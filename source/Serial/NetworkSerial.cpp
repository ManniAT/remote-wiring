/*
Copyright(c) Microsoft Open Technologies, Inc. All rights reserved.

The MIT License(MIT)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "pch.h"
#include "NetworkSerial.h"
#include <thread>

using namespace Concurrency;
using namespace Windows::Devices::Bluetooth::Rfcomm;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage::Streams;

using namespace Microsoft::Maker::Serial;

//******************************************************************************
//* Constructors
//******************************************************************************

NetworkSerial::NetworkSerial(
    Windows::Networking::HostName ^host_,
    uint16_t port_
    ) :
    _connection_ready( 0 ),
    _current_load_operation( nullptr ),
    _current_store_operation( nullptr ),
    _host( host_ ),
    _port( port_ ),
    _stream_socket( nullptr ),
    _rx( nullptr ),
    _tx( nullptr )
{
}

//******************************************************************************
//* Destructors
//******************************************************************************

NetworkSerial::~NetworkSerial(
    void
    )
{
    //we will fire the ConnectionLost event in the case that this object is unexpectedly destructed while the connection is established.
    if( connectionReady() )
    {
        ConnectionLost( L"Your connection has been terminated. The Microsoft::Maker::Serial::NetworkSerial destructor was called unexpectedly." );
    }
    end();
}

//******************************************************************************
//* Public Methods
//******************************************************************************

uint16_t
NetworkSerial::available(
    void
    )
{
    // Check to see if connection is ready
    if( !connectionReady() ) {
        return 0;
    }
    else {
        return _rx->UnconsumedBufferLength;
    }
}

/// \details Immediately discards the incoming parameters, because they are used for standard serial connections and will have no bearing on a network connection.
void
NetworkSerial::begin(
    uint32_t baud_,
    SerialConfig config_
    )
{
    // Discard incoming parameters inherited from IStream interface.
    UNREFERENCED_PARAMETER( baud_ );
    UNREFERENCED_PARAMETER( config_ );

    // Ensure known good state
    end();

    connectToHostAsync( _host, _port )
        .then( [ this ]( Concurrency::task<void> t )
    {
        try
        {
            t.get();
        }
        catch( Platform::Exception ^e )
        {
            ConnectionFailed( ref new Platform::String( L"NetworkSerial::connectAsync failed with a Platform::Exception type. Message: " ) + e->Message );
        }
        catch( ... )
        {
            ConnectionFailed( ref new Platform::String( L"NetworkSerial::connectAsync failed with a non-Platform::Exception type." ) );
        }
    } );
}

bool
NetworkSerial::connectionReady(
    void
    )
{
    return _connection_ready;
}

/// \ref https://social.msdn.microsoft.com/Forums/windowsapps/en-US/961c9d61-99ad-4a1b-82dc-22b6bd81aa2e/error-c2039-close-is-not-a-member-of-windowsstoragestreamsdatawriter?forum=winappswithnativecode
void
NetworkSerial::end(
    void
    )
{
    _connection_ready = false;
    _current_load_operation = nullptr;
    _current_store_operation = nullptr;

    // Reset with respect to dependencies
    delete( _rx );
    _rx = nullptr;
    delete( _tx );
    _tx = nullptr;
    delete( _stream_socket );
    _stream_socket = nullptr;
}

void
NetworkSerial::flush(
    void
    )
{
    _current_store_operation = _tx->StoreAsync();
    create_task( _current_store_operation )
        .then( [ this ]( unsigned int value_ )
    {
        return _tx->FlushAsync();
    } )
        .then( [ this ]( task<bool> task_ )
    {
        try
        {
            task_.get();

            //detect disconnection
            if( _current_store_operation->Status == Windows::Foundation::AsyncStatus::Error )
            {
                _connection_ready = false;
                ConnectionLost( L"A fatal error has occurred in NetworkSerial::flush() and your connection has been lost." );
            }
        }
        catch( Platform::Exception ^e )
        {
            _connection_ready = false;
            ConnectionLost( L"A fatal error has occurred in NetworkSerial::flush() and your connection has been lost. Error: " + e->Message );
        }
    } );
}

uint16_t
NetworkSerial::read(
    void
    )
{
    uint16_t c = static_cast<uint16_t>( -1 );

    if( !connectionReady() ) {
        return c;
    }

    if( available() ) {
        c = _rx->ReadByte();
    }
    else if( _current_load_operation->Status != Windows::Foundation::AsyncStatus::Started ) {
        // Attempt to detect disconnection
        if( _current_load_operation->Status == Windows::Foundation::AsyncStatus::Error )
        {
            _connection_ready = false;
            ConnectionLost( L"A fatal error has occurred in NetworkSerial::read() and your connection has been lost." );
            return -1;
        }

        _current_load_operation = _rx->LoadAsync( READ_CHUNK_SIZE );
    }

    return c;
}

uint32_t
NetworkSerial::write(
    uint8_t c_
    )
{
    // Check to see if connection is ready
    if( !connectionReady() ) { return 0; }

    _tx->WriteByte( c_ );
    return 1;
}

//******************************************************************************
//* Private Methods
//******************************************************************************

Concurrency::task<void>
NetworkSerial::connectToHostAsync(
    Windows::Networking::HostName ^host_,
    uint16_t port_
    )
{
    _stream_socket = ref new StreamSocket();
    _stream_socket->Control->KeepAlive = true;
    return Concurrency::create_task( _stream_socket->ConnectAsync( host_, port_.ToString() ) )
        .then( [ this ]()
    {
        _rx = ref new Windows::Storage::Streams::DataReader( _stream_socket->InputStream );
        _rx->InputStreamOptions = Windows::Storage::Streams::InputStreamOptions::Partial;  // Partial mode will allow for better async reads
        _current_load_operation = _rx->LoadAsync( READ_CHUNK_SIZE );

        // Enable TX
        _tx = ref new Windows::Storage::Streams::DataWriter( _stream_socket->OutputStream );
        _current_store_operation = nullptr;

        // Set connection ready flag
        _connection_ready = true;
        ConnectionEstablished();
    } );
}