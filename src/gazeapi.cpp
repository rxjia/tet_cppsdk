/*
 * Copyright (c) 2013-present, The Eye Tribe.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in
 * the LICENSE file in the root directory of this source tree.
 *
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#undef NOMINMAX
#endif

#include "gazeapi.h"
#include "gazeapi_interfaces.h"
#include "gazeapi_types.h"

#include "gazeapi_observable.hpp"
#include "gazeapi_parser.hpp"
#include "gazeapi_socket.hpp"

#define BOOST_SPIRIT_THREADSAFE
#include <boost/thread.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <sstream>
#include <iostream>
#include <vector>

namespace gtl
{
    class CalibrationProxy
    {
    public:
        CalibrationProxy()
            : m_point_count( 0 )
            , m_processed_points( 0 )
            , m_is_calibrating( false )
        {
        }

        void start_calibration( size_t const point_count )
        {
            m_point_count = point_count;
            m_is_calibrating = true;
        }

        void point_start()
        {
        }

        void point_end()
        {
            m_processed_points++;
        }

        bool is_done() const
        {
            return m_processed_points == m_point_count;
        }

        bool is_calibrating() const
        {
            return m_is_calibrating;
        }

        double get_progress() const
        {
            return 0 == m_point_count ? 0.0 : (double)m_processed_points / (double)m_point_count;
        }

        void clear()
        {
            m_point_count = 0;
            m_processed_points = 0;
            m_is_calibrating = false;
        }

    private:
        size_t m_point_count;
        size_t m_processed_points;
        bool m_is_calibrating;
    };


    class GazeApi::Engine
        : public ISocketListener
        , Observable<IGazeListener>
        , Observable<ICalibrationResultListener>
        , Observable<ITrackerStateListener>
        , Observable<ICalibrationProcessHandler>
        , Observable < IConnectionStateListener >
    {
    public:
        using Observable<IGazeListener>::add_observer;
        using Observable<IGazeListener>::remove_observer;
        using Observable<ICalibrationResultListener>::add_observer;
        using Observable<ICalibrationResultListener>::remove_observer;
        using Observable<ITrackerStateListener>::add_observer;
        using Observable<ITrackerStateListener>::remove_observer;
        using Observable<ICalibrationProcessHandler>::add_observer;
        using Observable<ICalibrationProcessHandler>::remove_observer;
        using Observable<IConnectionStateListener>::add_observer;
        using Observable<IConnectionStateListener>::remove_observer;

        Engine( int verbose_level = 0 )
            : m_socket( verbose_level )
            , m_state( AS_STOPPED )
        {
            m_socket.add_observer( *this );
        }

        virtual ~Engine()
        {
            disconnect();
            m_socket.remove_observer( *this );
        }

        bool is_running() const
        {
            return m_state == AS_RUNNING;
        }

        // GazeApi support
        bool connect( std::string const & host, std::string const & port )
        {
            if( AS_STOPPED != m_state )
            {
                return false;
            }
            m_host = host;
            m_port = port;
            m_sync_requests.clear();

            bool const success = m_socket.connect( m_host, m_port );

            if( success )
            {
                m_state = AS_RUNNING;

                memset( &m_server_proxy, 0, sizeof( ServerState ) );
                memset( &m_gaze_data, 0, sizeof( GazeData ) );
                memset( &m_screen, 0, sizeof( Screen ) );
                m_calib_result.clear();

                // Is this SDK version supported by the server?
                if( get_default_version() < VERSION )
                {
                    disconnect();
                    return false;
                }

                // Version 1: Initial version of C++ SDK uses a hacky way to synchronize API calls.
                //            EyeTribe server supported: all versions
                // Version 2: Optional id added to all API calls,
                //            and C++ SDK utilizes this new id feature to synchronize API calls robustly.
                //            EyeTribe server supported: from v0.9.53
                //
                // Set version 2
                if( !set_version( VERSION ) )
                {
                    disconnect();
                    return false;
                }

                Observable<IConnectionStateListener>::ObserverVector const & observers = Observable<IConnectionStateListener>::get_observers();
                for( size_t i = 0; i < observers.size(); ++i )
                {
                    observers[i]->on_connection_state_changed( true );
                }

                // retrieve current state
                get_tracker_state();
            }

            return success;
        }
        bool connect(std::string const & port )
        {
            return connect("127.0.0.1", port);
        }

        void disconnect()
        {
            if( m_state != AS_STOPPED )
            {
                m_state = AS_STOPPED;
                m_socket.disconnect();
            }
        }

        // This method is backwards compatible with all versions of the server API
        int get_default_version()
        {
            int version = m_server_proxy.version;
            m_server_proxy.version = 0;
            send_async( "{\"category\":\"tracker\",\"request\":\"get\",\"values\":[\"version\"]}" );
            size_t wait = 5000; // 5 secs
            while( m_server_proxy.version == 0 && wait > 0 )
            {
                boost::this_thread::sleep( boost::posix_time::milliseconds( 1 ) );
                --wait;
            }
            std::swap( version, m_server_proxy.version );
            return version;
        }

        bool set_version( size_t const version )
        {
            std::stringstream ss;
            ss << "{\"id\":" << SR_SET_VERSION << ",\"category\":\"tracker\",\"request\":\"set\",\"values\":" << "{\"version\":" << version << "}}";
            send_sync( ss.str() );
            return m_sync_requests[SR_SET_VERSION].is( GASC_OK );
        }

        bool set_screen( Screen const & screen )
        {
            std::stringstream ss;
            ss << "{\"id\":" << SR_SET_SCREEN << ",\"category\":\"tracker\",\"request\":\"set\",\"values\":" << "{\"screenindex\":" << screen.screenindex << ",\"screenresw\":" << screen.screenresw << ",\"screenresh\":" << screen.screenresh << ",\"screenpsyw\":" << screen.screenpsyw << ",\"screenpsyh\":" << screen.screenpsyh << "}}";
            send_sync( ss.str() );
            return m_sync_requests[SR_SET_SCREEN].is( GASC_OK );
        }

        void get_screen( Screen & screen ) const
        {
            m_screen_lock.lock();
            screen = m_screen;
            m_screen_lock.unlock();
        }

        void get_tracker_state()
        {
            // request everything
            std::stringstream ss;
            ss << "{\"id\":" << SR_GET_TRACKER_STATE << ","
                << "\"category\":\"tracker\",\"request\":\"get\",\"values\":["
                << "\"version\","
                << "\"trackerstate\","
                << "\"framerate\","
                << "\"iscalibrated\","
                << "\"iscalibrating\","
                << "\"calibresult\","
                << "\"frame\","
                << "\"screenindex\","
                << "\"screenresw\","
                << "\"screenresh\","
                << "\"screenpsyw\","
                << "\"screenpsyh\""
                << "]}";
         
           send_sync( ss.str() );
        }

        void get_frame( GazeData & gaze_data )
        {
            m_gaze_lock.lock();
            gaze_data = m_gaze_data;
            m_gaze_lock.unlock();
        }

        void get_calib_result( CalibResult & calib_result ) const
        {
            m_calib_lock.lock();
            calib_result = m_calib_result;
            m_calib_lock.unlock();
        }

        ServerState const & update_server_state()
        {
            get_tracker_state();
            return m_server_proxy;
        }

        ServerState const & get_server_state() const
        {
            return m_server_proxy;
        }

        bool calibration_start( int const point_count )
        {
            m_calibration_proxy.start_calibration( point_count );
            std::stringstream ss;
            ss << "{\"id\":" << SR_CALIB_START << ",\"category\":\"calibration\",\"request\":\"start\",\"values\":{\"pointcount\":" << point_count << "}}";
            send_sync( ss.str() );
            return m_sync_requests[SR_CALIB_START].is( GASC_OK );
        }

        void calibration_clear()
        {
            send_async( "{\"category\":\"calibration\",\"request\":\"clear\"}" );
        }

        void calibration_abort()
        {
            send_async( "{\"category\":\"calibration\",\"request\":\"abort\"}" );
        }

        bool calibration_point_start( int const x, int const y )
        {
            std::stringstream ss;
            ss << "{\"id\":" << SR_CALIB_POINT_START << ",\"category\":\"calibration\",\"request\":\"pointstart\",\"values\":{\"x\":" << x << ",\"y\":" << y << "}}";
            send_sync( ss.str() );
            return m_sync_requests[SR_CALIB_POINT_START].is( GASC_OK );
        }

        void calibration_point_end()
        {
            send_async( "{\"category\":\"calibration\",\"request\":\"pointend\"}" );
        }

        void on_message( std::string const & message )
        {
            try
            {
                Message msg;
                parse( msg, message );
                if( msg.has_id() )
                {
                    m_sync_requests[msg.m_id] = msg;
                }
            }
            catch( std::exception const & e )
            {
                e.what();
            }
        }

        void on_disconnected()
        {
            disconnect();

            Observable<IConnectionStateListener>::ObserverVector const & observers = Observable<IConnectionStateListener>::get_observers();
            for( size_t i = 0; i < observers.size(); ++i )
            {
                observers[i]->on_connection_state_changed( false );
            }
            // todo: try to reconnect here
        }

    private:

        struct Message
        {
            Message()
            {
                reset();
            }

            void reset()
            {
                m_category = GAC_UNKNOWN;
                m_request = GAR_UNKNOWN;
                m_statuscode = GASC_ERROR;
                m_id = -1;
                m_description.clear();
            }

            bool is( GazeApiCategory const & category ) const
            {
                return m_category == category;
            }

            bool is( GazeApiRequest const & request ) const
            {
                return m_request == request;
            }

            bool is( GazeApiStatusCode const & statuscode ) const
            {
                return m_statuscode == statuscode;
            }

            bool is_notification() const
            {
                return is( GASC_CALIBRATION_CHANGE ) || is( GASC_DISPLAY_CHANGE ) || is( GASC_TRACKER_STATE_CHANGE );
            }

            bool has_id() const
            {
                return m_id >= 0;
            }

            GazeApiCategory m_category;
            GazeApiRequest m_request;
            GazeApiStatusCode m_statuscode;
            int m_id;
            std::string m_description;
        };

    private:

        void send_sync( std::string const & message )
        {
            int const id = m_socket.get_id( message );
            if( m_state != AS_STOPPED && id != -1 )
            {
                m_sync_lock.lock();
                Message & msg = m_sync_requests[id];
                msg.reset();
                m_socket.send_sync( message );
                m_sync_lock.unlock();
            }
        }

        void send_async( std::string const & message )
        {
            if( m_state != AS_STOPPED )
            {
                m_socket.send( message );
            }
        }

        void parse( Message & reply, std::string const & json_message )
        {
            boost::property_tree::ptree root;
            {
                std::stringstream ss( json_message );
                boost::property_tree::read_json( ss, root );
            }

            reply = Message();

            // Parse message id and description if present
            Parser::parse_id( reply.m_id, root );
            Parser::parse_description( reply.m_description, root );

            if( !Parser::parse_category( reply.m_category, root ) )
            {
                return; // Broken message, so just ignore
            }

            if( !Parser::parse_status_code( reply.m_statuscode, root ) )
            {
                return; // Broken status code, so just ignore
            }

            // If message is broken or it's not a notification
            if( reply.is( GASC_ERROR ) && !reply.is_notification() )
            {
                // TODO: Maybe throw exception with description
                return;
            }

            // If message is notification we do not care about the request-part
            if( reply.is_notification() )
            {
                std::string values;
                switch( reply.m_statuscode )
                {
                    case GASC_CALIBRATION_CHANGE:
                    {
                        // initiate request for calibration data
                        values = "\"calibresult\",\"iscalibrated\",\"iscalibrating\"";
                        break;
                    }
                    case GASC_DISPLAY_CHANGE:
                    {
                        // initiate request for display index
                        values = "\"screenindex\",\"screenresw\",\"screenresh\",\"screenpsyw\",\"screenpsyh\"";
                        break;
                    }
                    case GASC_TRACKER_STATE_CHANGE:
                    {
                        // initiate request for tracker state
                        values = "\"trackerstate\"";
                        break;
                    }
                    default: break;
                }

                std::stringstream ss;
                ss << "{\"id\":" << SR_GET_CHANGES << ",\"category\":\"tracker\",\"request\":\"get\",\"values\":[" << values << "]}";
                send_sync( ss.str() );
                return;
            }

            // If message is NOT a notification we need to parse the request as well
            if( !reply.is_notification() && !Parser::parse_request( reply.m_request, root ) )
            {
                return; // Broken request, so just ignore
            }

            if( reply.is( GAC_TRACKER ) )
            {
                if( reply.is( GAR_SET ) )
                {
                    return; // Everything went fine so we just return;
                }
                else if( reply.is( GAR_GET ) )
                {
                    // Here we requested data, so we must process whatever is returned in "values"
                    bool has_gaze_data = false;
                    GazeData gaze_data;

                    bool has_calib_result = false;
                    CalibResult calib_result;

                    ServerState server_state = m_server_proxy;
                    Screen screen = m_screen;

                    if( !Parser::parse_server_state( server_state, gaze_data, calib_result, screen, root, has_gaze_data, has_calib_result ) )
                    {
                        return; // Parsing failed, so just return
                    }

                    bool const has_state_changed = server_state.trackerstate != m_server_proxy.trackerstate;

                    // Update everything
                    m_server_proxy = server_state;

                    if( has_gaze_data )
                    {
                        m_gaze_lock.lock();
                        m_gaze_data = gaze_data;
                        m_gaze_lock.unlock();

                        // There was gaze data present, so
                        typedef Observable<IGazeListener> ObservableType;
                        ObservableType::ObserverVector const & observers = ObservableType::get_observers();

                        for( size_t i = 0; i < observers.size(); ++i )
                        {
                            observers[i]->on_gaze_data( m_gaze_data );
                        }
                    }

                    if( has_calib_result )
                    {
                        m_calib_lock.lock();
                        m_calib_result = calib_result;
                        m_calib_lock.unlock();

                        typedef Observable<ICalibrationResultListener> ObservableType;
                        ObservableType::ObserverVector const & observers = ObservableType::get_observers();

                        for( size_t i = 0; i < observers.size(); ++i )
                        {
                            observers[i]->on_calibration_changed( m_calib_result.result, m_calib_result );
                        }
                    }

                    if( screen != m_screen )
                    {
                        m_screen_lock.lock();
                        m_screen = screen;
                        m_screen_lock.unlock();

                        typedef Observable<ITrackerStateListener> ObservableType;
                        ObservableType::ObserverVector const & observers = ObservableType::get_observers();

                        for( size_t i = 0; i < observers.size(); ++i )
                        {
                            observers[i]->on_screen_state_changed( m_screen );
                        }
                    }

                    if( has_state_changed )
                    {
                        typedef Observable<ITrackerStateListener> ObservableType;
                        ObservableType::ObserverVector const & observers = ObservableType::get_observers();

                        for( size_t i = 0; i < observers.size(); ++i )
                        {
                            observers[i]->on_tracker_connection_changed( m_server_proxy.trackerstate );
                        }
                    }
                }
                return;
            }

            if( reply.is( GAC_CALIBRATION ) )
            {
                if( reply.is( GAR_START ) )
                {
                    typedef Observable<ICalibrationProcessHandler> ObservableType;
                    ObservableType::ObserverVector const & observers = ObservableType::get_observers();

                    for( size_t i = 0; i < observers.size(); ++i )
                    {
                        observers[i]->on_calibration_started();
                    }
                }

                if( reply.is( GAR_POINTEND ) )
                {
                    m_calibration_proxy.point_end();

                    double progress = m_calibration_proxy.get_progress();

                    typedef Observable<ICalibrationProcessHandler> ObservableType;
                    ObservableType::ObserverVector const & observers = ObservableType::get_observers();

                    for( size_t i = 0; i < observers.size(); ++i )
                    {
                        observers[i]->on_calibration_progress( progress );
                    }

                    CalibResult calib_result;
                    bool has_calib_result;
                    if( !Parser::parse_calib_result( calib_result, root, has_calib_result ) )
                    {
                        return;
                    }

                    if( has_calib_result )
                    {
                        if( calib_result.result )
                        {
                            m_calib_lock.lock();
                            m_calib_result = calib_result;
                            m_calib_lock.unlock();

                            typedef Observable<ICalibrationResultListener> ObservableType;
                            ObservableType::ObserverVector const & observers = ObservableType::get_observers();

                            for( size_t i = 0; i < observers.size(); ++i )
                            {
                                observers[i]->on_calibration_changed( m_calib_result.result, m_calib_result );
                            }

                            m_calibration_proxy.clear();
                        }

                        typedef Observable<ICalibrationProcessHandler> ObservableType;
                        ObservableType::ObserverVector const & observers = ObservableType::get_observers();

                        for( size_t i = 0; i < observers.size(); ++i )
                        {
                            observers[i]->on_calibration_result( calib_result.result, calib_result );
                        }
                    }
                }

                if( reply.is( GAR_ABORT ) )
                {
                    m_calibration_proxy.clear();
                }

                if( reply.is( GAR_CLEAR ) )
                {
                    m_calib_lock.lock();
                    m_calib_result.clear();
                    m_calib_lock.unlock();
                }

            }
        }

    private:

        // Current API version this SDK requires!
        enum ApiVersion { VERSION = 2 };

        enum ApiState { AS_STOPPED, AS_RUNNING, AS_ISCALIBRATING };
        enum
        {
            SR_ERROR                = 1 << 0,
            SR_GET_TRACKER_STATE    = 1 << 1,
            SR_GET_FRAME            = 1 << 2,
            SR_GET_CALIB_RESULT     = 1 << 3,
            SR_GET_CHANGES          = 1 << 4,
            SR_SET_VERSION          = 1 << 5,
            SR_SET_SCREEN           = 1 << 7,
            SR_CALIB_START          = 1 << 8,
            SR_CALIB_POINT_START    = 1 << 9,
        };

        Socket                  m_socket;
        ApiState                m_state;
        CalibrationProxy        m_calibration_proxy;
        std::string             m_port;
        std::string             m_host;

        ServerState             m_server_proxy;
        GazeData                m_gaze_data;
        CalibResult             m_calib_result;
        Screen                  m_screen;
        std::map<int, Message>  m_sync_requests;

        mutable boost::mutex    m_calib_lock;
        mutable boost::mutex    m_gaze_lock;
        mutable boost::mutex    m_screen_lock;
        mutable boost::mutex    m_sync_lock;
    };

    GazeApi::GazeApi( int verbose_level )
        : m_engine( new Engine( verbose_level ) )
    {
    }

    GazeApi::~GazeApi()
    {
    }

    void GazeApi::add_listener( IGazeListener & listener )
    {
        m_engine->add_observer( listener );
    }

    void GazeApi::remove_listener( IGazeListener & listener )
    {
        m_engine->remove_observer( listener );
    }

    void GazeApi::add_listener( ICalibrationResultListener & listener )
    {
        m_engine->add_observer( listener );
    }

    void GazeApi::remove_listener( ICalibrationResultListener & listener )
    {
        m_engine->remove_observer( listener );
    }

    void GazeApi::add_listener( IConnectionStateListener & listener )
    {
        m_engine->add_observer( listener );
    }

    void GazeApi::remove_listener( IConnectionStateListener & listener )
    {
        m_engine->remove_observer( listener );
    }

    void GazeApi::add_listener( ITrackerStateListener & listener )
    {
        m_engine->add_observer( listener );
    }

    void GazeApi::remove_listener( ITrackerStateListener & listener )
    {
        m_engine->remove_observer( listener );
    }

    void GazeApi::add_listener( ICalibrationProcessHandler & listener )
    {
        m_engine->add_observer( listener );
    }

    void GazeApi::remove_listener( ICalibrationProcessHandler & listener )
    {
        m_engine->remove_observer( listener );
    }

    bool GazeApi::is_connected() const
    {
        return m_engine->is_running();
    }

    bool GazeApi::connect()
    {
        return connect( 6555 );
    }

    bool GazeApi::connect( unsigned short port )
    {
        std::stringstream ss;
        ss << port;
        return m_engine->connect( ss.str() );
    }

    bool GazeApi::connect( std::string const & host, unsigned short port )
    {
        std::stringstream ss;
        ss << port;
        return m_engine->connect( host, ss.str() );
    }

    void GazeApi::disconnect()
    {
        m_engine->disconnect();
    }

    bool GazeApi::set_screen( Screen const & screen )
    {
        return m_engine->set_screen( screen );
    }

    void GazeApi::get_screen( Screen & screen ) const
    {
        m_engine->get_screen( screen );
    }

    void GazeApi::get_frame( GazeData & gaze_data ) const
    {
        m_engine->get_frame( gaze_data );
    }

    void GazeApi::get_calib_result( CalibResult & calib_result ) const
    {
        m_engine->get_calib_result( calib_result );
    }

    ServerState const & GazeApi::update_server_state()
    {
        return m_engine->update_server_state();
    }

    ServerState const & GazeApi::get_server_state() const
    {
        return m_engine->get_server_state();
    }

    bool GazeApi::calibration_start( int const point_count )
    {
        return m_engine->calibration_start( point_count );
    }

    void GazeApi::calibration_clear()
    {
        m_engine->calibration_clear();
    }

    void GazeApi::calibration_abort()
    {
        m_engine->calibration_abort();
    }

    bool GazeApi::calibration_point_start( int const x, int const y )
    {
        return m_engine->calibration_point_start( x, y );
    }

    void GazeApi::calibration_point_end()
    {
        m_engine->calibration_point_end();
    }

} // namespace gtl