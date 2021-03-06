/*****************************************************************************
 * chromecast_communication.cpp: Handle chromecast protocol messages
 *****************************************************************************
 * Copyright © 2014-2017 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
 *          Hugo Beauzée-Luyssen <hugo@beauzee.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chromecast.h"
#ifdef HAVE_POLL
# include <poll.h>
#endif

/* deadline regarding pings sent from receiver */
#define PING_WAIT_TIME 6000

ChromecastCommunication::ChromecastCommunication( vlc_object_t* p_module, const char* targetIP, unsigned int devicePort )
    : m_module( p_module )
    , m_creds( NULL )
    , m_tls( NULL )
    , m_receiver_requestId( 0 )
    , m_requestId( 0 )
{
    if (devicePort == 0)
        devicePort = CHROMECAST_CONTROL_PORT;

    m_creds = vlc_tls_ClientCreate( m_module->obj.parent );
    if (m_creds == NULL)
        throw std::runtime_error( "Failed to create TLS client" );

    m_tls = vlc_tls_SocketOpenTLS( m_creds, targetIP, devicePort, "tcps",
                                   NULL, NULL );
    if (m_tls == NULL)
    {
        vlc_tls_Delete(m_creds);
        throw std::runtime_error( "Failed to create client session" );
    }

    char psz_localIP[NI_MAXNUMERICHOST];
    if (net_GetSockAddress( vlc_tls_GetFD(m_tls), psz_localIP, NULL ))
        throw std::runtime_error( "Cannot get local IP address" );

    m_serverIp = psz_localIP;
}

ChromecastCommunication::~ChromecastCommunication()
{
    disconnect();
}

void ChromecastCommunication::disconnect()
{
    if ( m_tls != NULL )
    {
        vlc_tls_Close(m_tls);
        vlc_tls_Delete(m_creds);
        m_tls = NULL;
    }
}

/**
 * @brief Build a CastMessage to send to the Chromecast
 * @param namespace_ the message namespace
 * @param payloadType the payload type (CastMessage_PayloadType_STRING or
 * CastMessage_PayloadType_BINARY
 * @param payload the payload
 * @param destinationId the destination idenifier
 * @return the generated CastMessage
 */
void ChromecastCommunication::buildMessage(const std::string & namespace_,
                              const std::string & payload,
                              const std::string & destinationId,
                              castchannel::CastMessage_PayloadType payloadType)
{
    castchannel::CastMessage msg;

    msg.set_protocol_version(castchannel::CastMessage_ProtocolVersion_CASTV2_1_0);
    msg.set_namespace_(namespace_);
    msg.set_payload_type(payloadType);
    msg.set_source_id("sender-vlc");
    msg.set_destination_id(destinationId);
    if (payloadType == castchannel::CastMessage_PayloadType_STRING)
        msg.set_payload_utf8(payload);
    else // CastMessage_PayloadType_BINARY
        msg.set_payload_binary(payload);

    sendMessage(msg);
}

/**
 * @brief Receive a data packet from the Chromecast
 * @param p_data the buffer in which to store the data
 * @param i_size the size of the buffer
 * @param i_timeout maximum time to wait for a packet, in millisecond
 * @param pb_timeout Output parameter that will contain true if no packet was received due to a timeout
 * @return the number of bytes received of -1 on error
 */
ssize_t ChromecastCommunication::receive( uint8_t *p_data, size_t i_size, int i_timeout, bool *pb_timeout )
{
    ssize_t i_received = 0;
    struct pollfd ufd[1];
    ufd[0].fd = vlc_tls_GetFD( m_tls );
    ufd[0].events = POLLIN;

    struct iovec iov;
    iov.iov_base = p_data;
    iov.iov_len = i_size;

    /* The Chromecast normally sends a PING command every 5 seconds or so.
     * If we do not receive one after 6 seconds, we send a PING.
     * If after this PING, we do not receive a PONG, then we consider the
     * connection as dead. */
    do
    {
        ssize_t i_ret = m_tls->readv( m_tls, &iov, 1 );
        if ( i_ret < 0 )
        {
#ifdef _WIN32
            if ( WSAGetLastError() != WSAEWOULDBLOCK )
#else
            if ( errno != EAGAIN )
#endif
            {
                return -1;
            }
            ssize_t val = vlc_poll_i11e(ufd, 1, i_timeout);
            if ( val < 0 )
                return -1;
            else if ( val == 0 )
            {
                *pb_timeout = true;
                return i_received;
            }
            assert( ufd[0].revents & POLLIN );
            continue;
        }
        else if ( i_ret == 0 )
            return -1;
        assert( i_size >= (size_t)i_ret );
        i_size -= i_ret;
        i_received += i_ret;
        iov.iov_base = (uint8_t*)iov.iov_base + i_ret;
        iov.iov_len = i_size;
    } while ( i_size > 0 );
    return i_received;
}


/*****************************************************************************
 * Message preparation
 *****************************************************************************/
void ChromecastCommunication::msgAuth()
{
    castchannel::DeviceAuthMessage authMessage;
    authMessage.mutable_challenge();

    buildMessage(NAMESPACE_DEVICEAUTH, authMessage.SerializeAsString(),
                 DEFAULT_CHOMECAST_RECEIVER, castchannel::CastMessage_PayloadType_BINARY);
}


void ChromecastCommunication::msgPing()
{
    std::string s("{\"type\":\"PING\"}");
    buildMessage( NAMESPACE_HEARTBEAT, s, DEFAULT_CHOMECAST_RECEIVER );
}


void ChromecastCommunication::msgPong()
{
    std::string s("{\"type\":\"PONG\"}");
    buildMessage( NAMESPACE_HEARTBEAT, s, DEFAULT_CHOMECAST_RECEIVER );
}

void ChromecastCommunication::msgConnect( const std::string& destinationId )
{
    std::string s("{\"type\":\"CONNECT\"}");
    buildMessage( NAMESPACE_CONNECTION, s, destinationId );
}

void ChromecastCommunication::msgReceiverClose( const std::string& destinationId )
{
    std::string s("{\"type\":\"CLOSE\"}");
    buildMessage( NAMESPACE_CONNECTION, s, destinationId );
}

void ChromecastCommunication::msgReceiverGetStatus()
{
    std::stringstream ss;
    ss << "{\"type\":\"GET_STATUS\","
       <<  "\"requestId\":" << m_receiver_requestId++ << "}";

    buildMessage( NAMESPACE_RECEIVER, ss.str(), DEFAULT_CHOMECAST_RECEIVER );
}

void ChromecastCommunication::msgReceiverLaunchApp()
{
    std::stringstream ss;
    ss << "{\"type\":\"LAUNCH\","
       <<  "\"appId\":\"" << APP_ID << "\","
       <<  "\"requestId\":" << m_receiver_requestId++ << "}";

    buildMessage( NAMESPACE_RECEIVER, ss.str(), DEFAULT_CHOMECAST_RECEIVER );
}

void ChromecastCommunication::msgPlayerGetStatus( const std::string& destinationId )
{
    std::stringstream ss;
    ss << "{\"type\":\"GET_STATUS\","
       <<  "\"requestId\":" << m_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

std::string ChromecastCommunication::GetMedia( unsigned int i_port,
                                               const std::string& mime,
                                               const vlc_meta_t *p_meta )
{
    std::stringstream ss;

    bool b_music = strncmp(mime.c_str(), "audio", strlen("audio")) == 0;

    const char *psz_title = NULL;
    const char *psz_artwork = NULL;
    const char *psz_artist = NULL;
    const char *psz_album = NULL;
    const char *psz_albumartist = NULL;
    const char *psz_tracknumber = NULL;
    const char *psz_discnumber = NULL;

    if( p_meta )
    {
        psz_title = vlc_meta_Get( p_meta, vlc_meta_Title );
        psz_artwork = vlc_meta_Get( p_meta, vlc_meta_ArtworkURL );

        if( b_music && psz_title )
        {
            psz_artist = vlc_meta_Get( p_meta, vlc_meta_Artist );
            psz_album = vlc_meta_Get( p_meta, vlc_meta_Album );
            psz_albumartist = vlc_meta_Get( p_meta, vlc_meta_AlbumArtist );
            psz_tracknumber = vlc_meta_Get( p_meta, vlc_meta_TrackNumber );
            psz_discnumber = vlc_meta_Get( p_meta, vlc_meta_DiscNumber );
        }
        if( !psz_title )
        {
            psz_title = vlc_meta_Get( p_meta, vlc_meta_NowPlaying );
            if( !psz_title )
                psz_title = vlc_meta_Get( p_meta, vlc_meta_ESNowPlaying );
        }

        if ( psz_title )
        {
            ss << "\"metadata\":{"
               << " \"metadataType\":" << ( b_music ? "3" : "0" )
               << ",\"title\":\"" << psz_title << "\"";
            if( b_music )
            {
                if( psz_artist )
                    ss << ",\"artist\":\"" << psz_artist << "\"";
                if( psz_album )
                    ss << ",\"album\":\"" << psz_album << "\"";
                if( psz_albumartist )
                    ss << ",\"albumArtist\":\"" << psz_albumartist << "\"";
                if( psz_tracknumber )
                    ss << ",\"trackNumber\":\"" << psz_tracknumber << "\"";
                if( psz_discnumber )
                    ss << ",\"discNumber\":\"" << psz_discnumber << "\"";
            }

            if ( psz_artwork && !strncmp( psz_artwork, "http", 4 ) )
                ss << ",\"images\":[{\"url\":\"" << psz_artwork << "\"}]";

            ss << "},";
        }
    }

    std::stringstream chromecast_url;
    chromecast_url << "http://" << m_serverIp << ":" << i_port << "/stream";

    msg_Dbg( m_module, "s_chromecast_url: %s", chromecast_url.str().c_str());

    ss << "\"contentId\":\"" << chromecast_url.str() << "\""
       << ",\"streamType\":\"LIVE\""
       << ",\"contentType\":\"" << mime << "\"";

    return ss.str();
}

void ChromecastCommunication::msgPlayerLoad( const std::string& destinationId, unsigned int i_port,
                                             const std::string& mime, const vlc_meta_t *p_meta )
{
    std::stringstream ss;
    ss << "{\"type\":\"LOAD\","
       <<  "\"media\":{" << GetMedia( i_port, mime, p_meta ) << "},"
       <<  "\"autoplay\":\"false\","
       <<  "\"requestId\":" << m_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerPlay( const std::string& destinationId, int64_t mediaSessionId )
{
    assert(mediaSessionId != 0);

    std::stringstream ss;
    ss << "{\"type\":\"PLAY\","
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << m_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerStop( const std::string& destinationId, int64_t mediaSessionId )
{
    assert(mediaSessionId != 0);

    std::stringstream ss;
    ss << "{\"type\":\"STOP\","
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << m_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerPause( const std::string& destinationId, int64_t mediaSessionId )
{
    assert(mediaSessionId != 0);

    std::stringstream ss;
    ss << "{\"type\":\"PAUSE\","
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << m_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerSetVolume( const std::string& destinationId, int64_t mediaSessionId, float f_volume, bool b_mute )
{
    assert(mediaSessionId != 0);

    if ( f_volume < 0.0 || f_volume > 1.0)
        return;

    std::stringstream ss;
    ss << "{\"type\":\"SET_VOLUME\","
       <<  "\"volume\":{\"level\":" << f_volume << ",\"muted\":" << ( b_mute ? "true" : "false" ) << "},"
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << m_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

void ChromecastCommunication::msgPlayerSeek( const std::string& destinationId, int64_t mediaSessionId, const std::string& currentTime )
{
    assert(mediaSessionId != 0);

    std::stringstream ss;
    ss << "{\"type\":\"SEEK\","
       <<  "\"currentTime\":" << currentTime << ","
       <<  "\"mediaSessionId\":" << mediaSessionId << ","
       <<  "\"requestId\":" << m_requestId++
       << "}";

    pushMediaPlayerMessage( destinationId, ss );
}

/**
 * @brief Send a message to the Chromecast
 * @param msg the CastMessage to send
 * @return vlc error code
 */
int ChromecastCommunication::sendMessage( const castchannel::CastMessage &msg )
{
    int i_size = msg.ByteSize();
    uint8_t *p_data = new(std::nothrow) uint8_t[PACKET_HEADER_LEN + i_size];
    if (p_data == NULL)
        return VLC_ENOMEM;

#ifndef NDEBUG
    msg_Dbg( m_module, "sendMessage: %s->%s %s", msg.namespace_().c_str(), msg.destination_id().c_str(), msg.payload_utf8().c_str());
#endif

    SetDWBE(p_data, i_size);
    msg.SerializeWithCachedSizesToArray(p_data + PACKET_HEADER_LEN);

    int i_ret = vlc_tls_Write(m_tls, p_data, PACKET_HEADER_LEN + i_size);
    delete[] p_data;
    if (i_ret == PACKET_HEADER_LEN + i_size)
        return VLC_SUCCESS;

    msg_Warn( m_module, "failed to send message %s (%s)", msg.payload_utf8().c_str(), strerror( errno ) );

    return VLC_EGENERIC;
}

void ChromecastCommunication::pushMediaPlayerMessage( const std::string& destinationId, const std::stringstream & payload )
{
    assert(!destinationId.empty());
    buildMessage( NAMESPACE_MEDIA, payload.str(), destinationId );
}
