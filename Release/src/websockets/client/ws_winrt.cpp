/***
* ==++==
*
* Copyright (c) Microsoft Corporation. All rights reserved. 
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* ==--==
* =+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
*
* ws_winrt.cpp
*
* Websocket library: Client-side APIs.
* 
* This file contains the implementation for Windows 8 
*
* =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
****/
#include "stdafx.h"

#if WINAPI_FAMILY == WINAPI_FAMILY_APP

using namespace ::Windows::Foundation;
using namespace ::Windows::Storage;
using namespace ::Windows::Storage::Streams;
using namespace ::Windows::Networking;
using namespace ::Windows::Networking::Sockets;
using namespace Concurrency::streams::details;

namespace web 
{
namespace experimental
{
namespace web_sockets
{
namespace client
{
namespace details
{

// This class is required by the implementation in order to function:
// The TypedEventHandler requires the message received and close handler to be a member of WinRT class.
ref class ReceiveContext sealed
{
public:
    ReceiveContext() {}

    friend class winrt_client;
    void OnReceive(MessageWebSocket^ sender, MessageWebSocketMessageReceivedEventArgs^ args);
    void OnClosed(IWebSocket^ sender, WebSocketClosedEventArgs^ args);

private:
    // Public members cannot have native types
    ReceiveContext(std::function<void(std::shared_ptr<websocket_incoming_message>)> receive_handler, std::function<void()> close_handler): m_receive_handler(receive_handler), m_close_handler(close_handler) {}

    // Handler to be executed when a message has been received by the client
    std::function<void(std::shared_ptr<websocket_incoming_message>)> m_receive_handler;

    // Handler to be executed when a close message has been received by the client
    std::function<void()> m_close_handler;
};

class winrt_client : public _websocket_client_impl
{
public:
    winrt_client(web::uri address, websocket_client_config client_config)
        : _websocket_client_impl(std::move(address), std::move(client_config)), m_scheduled(0)
    {
        verify_uri(m_uri);
        m_msg_websocket = ref new MessageWebSocket();

        switch(client_config.message_type())
        {
        case websocket_message_type::binary_message :
            m_msg_websocket->Control->MessageType = SocketMessageType::Binary;
            break;
        case websocket_message_type::text_message:
            m_msg_websocket->Control->MessageType = SocketMessageType::Utf8;
            break;
        default:
            throw std::invalid_argument("Invalid message type: neither UTF-8 nor binary!");
        }

        m_context = ref new ReceiveContext([=](std::shared_ptr<websocket_incoming_message> msg)
        {
            std::unique_lock<std::mutex> lock(m_receive_queue_lock);
            if (msg != nullptr)
                m_receive_msg_queue.push(std::move(*msg));
            m_receive_msg_cv.notify_one();
        }, 
        [=]()
        {
            m_receive_msg_cv.notify_all();
            m_close_tce.set();
        }); 
    }

    ~winrt_client()
    {
        m_receive_msg_cv.notify_all();
    }

    pplx::task<void> connect()
    {
        const auto uri = ref new Windows::Foundation::Uri(ref new Platform::String(m_uri.to_string().c_str()));

        m_msg_websocket->MessageReceived += ref new TypedEventHandler<MessageWebSocket^, MessageWebSocketMessageReceivedEventArgs^>(m_context, &ReceiveContext::OnReceive);
        m_msg_websocket->Closed += ref new TypedEventHandler<IWebSocket^, WebSocketClosedEventArgs^>(m_context, &ReceiveContext::OnClosed);

        return pplx::create_task(m_msg_websocket->ConnectAsync(uri)).then([=](pplx::task<void> result) -> pplx::task<void>
        {
            try
            {
                result.get();
                m_messageWriter = ref new DataWriter(m_msg_websocket->OutputStream);
            }
            catch(Platform::Exception^ ex)
            {
                return pplx::task_from_exception<void>(websocket_exception(ex->HResult));
            }
            return pplx::task_from_result();
        });
    }

    pplx::task<void> send(websocket_outgoing_message msg)
    {
        if (m_messageWriter == nullptr)
        {
            return pplx::task_from_exception<void>(websocket_exception(_XPLATSTR("Client not connected.")));
        }

        if (msg._m_impl->message_type() != config().message_type())
        {
            return pplx::task_from_exception<void>(websocket_exception(_XPLATSTR("Message type mismatch.")));
        }

        const auto length = msg._m_impl->length();
        if (length == 0)
        {
            return pplx::task_from_exception<void>(websocket_exception(_XPLATSTR("Cannot send empty message.")));
        }

        if (length > UINT_MAX)
        {
            return pplx::task_from_exception<void>(websocket_exception(_XPLATSTR("Message size too large. Ensure message length is less than or equal to UINT_MAX.")));
        }

        {
            std::unique_lock<std::mutex> lock(m_send_lock);
            ++m_scheduled;
            if (m_scheduled == 1) // No sends in progress
            {
                // Start sending the message
                send_msg(msg);
            }
            else
            {
                m_outgoing_msg_queue.push(msg);
            }
        }
        return pplx::create_task(msg.m_send_tce);
    }

    void send_msg(websocket_outgoing_message msg)
    {
        auto length = msg._m_impl->length();

        auto is = msg._m_impl->instream();
        auto is_buf = is.streambuf();

        std::shared_ptr<uint8_t> sp( new uint8_t[length](), []( uint8_t *p ) { delete[] p; } );
        is_buf.getn(sp.get(), length).then([this, sp, msg](size_t bytes_read)
        {   
            m_messageWriter->WriteBytes(Platform::ArrayReference<unsigned char>(sp.get(), static_cast<unsigned int>(bytes_read)));

            // Send the data as one complete message, in WinRT we do not have an option to send fragments
            return pplx::task<unsigned int>(m_messageWriter->StoreAsync()).then([this, msg, bytes_read] (unsigned int bytes_written) 
            {
                if (bytes_written != bytes_read)
                {
                    msg.m_send_tce.set_exception(std::make_exception_ptr(websocket_exception(_XPLATSTR("Failed to send all the bytes."))));
                }            
            });
        }).then([this, msg] (pplx::task<void> previousTask)
        {
            try
            { 
                // Get the exception from DataWriter::StoreAsync, if any and convert it to websocket exception
                previousTask.get();
            }
            catch (Platform::Exception^ exception)
            {
                msg.m_send_tce.set_exception(std::make_exception_ptr(websocket_exception(exception->HResult)));
            }

            {
                std::unique_lock<std::mutex> lock(m_send_lock);
                --m_scheduled;
                if (!m_outgoing_msg_queue.empty())
                {
                    auto next_msg = m_outgoing_msg_queue.front();
                    m_outgoing_msg_queue.pop();
                    send_msg(next_msg);
                }
            }
            msg.m_send_tce.set();
        });
    }

    pplx::task<websocket_incoming_message> receive()
    {
        return pplx::create_task([this]() ->pplx::task<websocket_incoming_message>
        {
            std::unique_lock<std::mutex> lock(m_receive_queue_lock);
            m_receive_msg_cv.wait(lock);
            if (m_receive_msg_queue.empty()) // If queue is empty -> wait was notified by websocket client close or destructor etc.
            {
                return pplx::task_from_exception<websocket_incoming_message>(websocket_exception(_XPLATSTR("Error receiving message, websocket client is closing.")));
            }
            auto msg = std::move(m_receive_msg_queue.front());
            m_receive_msg_queue.pop();
            return pplx::task_from_result<websocket_incoming_message>(msg);
        });
    }

    pplx::task<void> close()
    {
        // Send a close frame to the server
        return close(websocket_close_status::normal);
    }

    pplx::task<void> close(websocket_close_status status, const utility::string_t &strreason=_XPLATSTR("")) 
    {
        // Send a close frame to the server 
        Platform::String^ reason = ref new Platform::String(strreason.data());
        m_msg_websocket->Close(static_cast<unsigned short>(status), reason);
        // Wait for the close response frame from the server.
        return pplx::create_task(m_close_tce);
    }

    static std::shared_ptr<details::_websocket_message> get_impl(websocket_incoming_message msg)
    {
        return msg._m_impl;
    }

private:

    // WinRT MessageWebSocket object
    Windows::Networking::Sockets::MessageWebSocket^ m_msg_websocket;
    Windows::Storage::Streams::DataWriter^ m_messageWriter;
    // Context object that implements the WinRT handlers: receive handler and close handler
    ReceiveContext ^ m_context;

    pplx::task_completion_event<void> m_close_tce;
    
    // Incoming messages are maintained in a producer consumer queue. 
    // m_receive_queue_lock and m_receive_msg_cv : synchronization primitives to guard access to the queue
    std::mutex m_receive_queue_lock;
    std::condition_variable m_receive_msg_cv;
    std::queue<websocket_incoming_message> m_receive_msg_queue;

    // The implementation has to ensure ordering of send requests
    std::mutex m_send_lock; 

    // Queue to order the sends
    std::queue<websocket_outgoing_message> m_outgoing_msg_queue;

    // Number of scheduled sends
    int m_scheduled; 
};

void ReceiveContext::OnReceive(MessageWebSocket^ sender, MessageWebSocketMessageReceivedEventArgs^ args)
{
    websocket_incoming_message ws_incoming_message;
    auto msg = winrt_client::get_impl(ws_incoming_message);
    msg->_prepare_to_receive_data();

    switch(args->MessageType)
    {
    case SocketMessageType::Binary:
        msg->set_msg_type(websocket_message_type::binary_message);
        break;
    case SocketMessageType::Utf8:
        msg->set_msg_type(websocket_message_type::text_message);
        break;
    }

    auto outstream = msg->outstream();

    auto writebuf = outstream.streambuf();

    try
    {
        DataReader^ reader = args->GetDataReader();
        auto len = reader->UnconsumedBufferLength;
        auto block = writebuf.alloc(len);
        reader->ReadBytes(Platform::ArrayReference<uint8>(block, len)); 
        writebuf.commit(len);
        writebuf.close(std::ios::out).wait(); // Since this is an in-memory stream, this call is not blocking.
        msg->set_length(len);
        msg->_set_data_available(); 
        m_receive_handler(std::make_shared<websocket_incoming_message>(ws_incoming_message));
    }
    catch(Platform::Exception^ ex)
    {
        m_receive_handler(nullptr);
    }
}

void ReceiveContext::OnClosed(IWebSocket^ sender, WebSocketClosedEventArgs^ args)
{
    m_close_handler();
}
}

websocket_client::websocket_client(web::uri base_uri)
:m_client(std::make_shared<details::winrt_client>(std::move(base_uri), websocket_client_config()))
{
}

websocket_client::websocket_client(web::uri base_uri, websocket_client_config config)
:m_client(std::make_shared<details::winrt_client>(std::move(base_uri), std::move(config)))
{
}

}}}}
#endif /* WINAPI_FAMILY == WINAPI_FAMILY_APP */