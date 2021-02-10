//
// Created by rs on 2021/2/9.
//

#include <gazeapi.h>
#include <iostream>
#include <armadillo>

using namespace std;

class ITracker : public gtl::ITrackerStateListener {
public:
    ITracker() {};

    ~ITracker() {};

    int tracker_state;

    const string tStateToStr(int tracker_state) {
        switch (tracker_state) {
            case gtl::ServerState::TRACKER_CONNECTED:
                return "TRACKER_CONNECTED";
            case gtl::ServerState::TRACKER_NOT_CONNECTED:
                return "TRACKER_NOT_CONNECTED";
            case gtl::ServerState::TRACKER_CONNECTED_BADFW:
                return "TRACKER_CONNECTED_BADFW";
            case gtl::ServerState::TRACKER_CONNECTED_NOUSB3:
                return "TRACKER_CONNECTED_NOUSB3";
            case gtl::ServerState::TRACKER_CONNECTED_NOSTREAM:
                return "TRACKER_CONNECTED_NOSTREAM";
            default:
                string s = "TRACKER_ERR: ";
                s += to_string(tracker_state);
                return s;
        }
    }

    void on_tracker_connection_changed(int tracker_state) {
        cout << "## connection_changed: " << tStateToStr(this->tracker_state) << " -> " << tStateToStr(tracker_state)
             << endl;
        this->tracker_state = tracker_state;
    }

    void on_screen_state_changed(gtl::Screen const &screen) {
        cout << "## on_screen_state_changed" << " " << screen.screenindex << endl;
    }
};

// --- MyGaze definition
class MyGaze : public gtl::IGazeListener {
public:
    MyGaze();

    ~MyGaze();

private:
    // IGazeListener
    void on_gaze_data(gtl::GazeData const &gaze_data);

private:
    gtl::GazeApi m_api;
    ITracker iTracker;
};

// --- MyGaze implementation
MyGaze::MyGaze()
        : m_api(0) // verbose_level 0 (disabled)
{
    // Connect to the server on the default TCP port (6555)
    if (m_api.connect(6555)) {
        cout << "connect success" << endl;
        // Enable GazeData notifications
        m_api.add_listener(*this);
        m_api.add_listener(iTracker);
    } else
        cout << "connect failed" << endl;
}

MyGaze::~MyGaze() {
    m_api.remove_listener(iTracker);
    m_api.remove_listener(*this);
    m_api.disconnect();
}

void MyGaze::on_gaze_data(gtl::GazeData const &gaze_data) {
    cout << gaze_data.time << " " << gaze_data.state << "\t - ";

    for (int i = 0; i < 5; i++)
        cout << (gaze_data.state & (1 << (4 - i))) << " ";
    cout << "fix: " << gaze_data.fix << " " << "raw: " << gaze_data.raw.x << " " << gaze_data.raw.y << "" << endl;

    if (gaze_data.state & gtl::GazeData::GD_STATE_TRACKING_GAZE) {
        gtl::Point2D const &smoothedCoordinates = gaze_data.avg;
        // Move GUI point, do hit-testing, log coordinates, etc.
    }
}

int main() {
    std::cout << "Hello, World!" << std::endl;
    MyGaze myGaze;
    while (1)
        sleep(5);
    return 0;
}