#pragma once

/*
Copyright 2022 Alexander Ranaldi
W2AXR
alexranaldi@gmail.com

This file is part of CWSL_DIGI.

CWSL_DIGI is free software : you can redistribute it and /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

CWSL_DIGI is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with CWSL_DIGI. If not, see < https://www.gnu.org/licenses/>.
*/

#include <cstdint>
#include <chrono>
#include <list>
#include <thread>
#include <atomic>

#include <iostream>
#include <iomanip>
#include <ios>

#include "SafeQueue.h"
#include "ScreenPrinter.hpp"

#include <windows.h>
#include <winsock.h>
# pragma comment(lib, "Ws2_32.lib")

#include "CWSL_DIGI.hpp"

using namespace std::literals; 
using namespace std;

namespace wspr {
    struct Report
    {
        std::string callsign;
        std::int32_t snr;
        std::uint32_t freq;
        std::string locator;
        std::uint64_t epochTime;
        std::string mode;
        float dt;
        std::int16_t drift;
        std::uint32_t recvfreq;
        std::int16_t dbm;
        std::string reporterCallsign;
    };
}

class WSPRNet {

public:
    WSPRNet(const std::string& grid, std::shared_ptr<ScreenPrinter> sp ) :
    operatorGrid(grid),
    screenPrinter(sp),
    mCountSendsErrored(0),
    mCountSendsOK(0),
    terminateFlag(false) {
    }

    virtual ~WSPRNet() {
        WSACleanup();
    }

    bool init() {

        WSADATA wsaData;
        int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (res != NO_ERROR) {
            std::cerr << "WSAStartup failed" << std::endl;
            return false;
        }

        target.sin_family = AF_INET;
        target.sin_port = htons(HTTP_PORT);
        target.sin_addr.s_addr = inet_addr(SERVER_IP.c_str());

        mSendThread = std::thread(&WSPRNet::processingLoop, this);
        SetThreadPriority(mSendThread.native_handle(), THREAD_PRIORITY_IDLE);
        mSendThread.detach();
        return true;
    }

    void terminate() {
        screenPrinter->debug("WSPRNet interface terminating");
        terminateFlag = true;
    }

    void handle(const std::string callsign, const int32_t snr, const float dt, const std::int16_t drift, const std::int16_t dbm, const uint32_t freq, const uint32_t rf, const uint64_t epochTime, const std::string& grid, const std::string& reporterCallsign) {

        wspr::Report rep;
        rep.callsign = callsign;
        rep.snr = snr;
        rep.freq = freq;
        rep.locator = grid;
        rep.epochTime = epochTime;
        rep.dt = dt;
        rep.drift = drift;
        rep.recvfreq = rf;
        rep.dbm = dbm;
        rep.reporterCallsign = reporterCallsign;
 
        mReports.enqueue(rep);
    }

    bool isConnected() {
        int error = 0;
        int len = sizeof(error);
        int retval = getsockopt(mSocket, SOL_SOCKET, SO_ERROR, (char*)&error, &len);

        if (retval != 0) {
            return false;
        }

        if (error != 0) {
            return false;
        }

        return true;
    }

    bool closeSocket() {
        int closeStatus = closesocket(mSocket);
        if (0 != closeStatus) {
            screenPrinter->print("Error closing socket", LOG_LEVEL::ERR);
            return false;
        }
        return true;
    }

    bool connectSocket() {
        mSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (mSocket == INVALID_SOCKET) {
            screenPrinter->print("Socket creation failed with error: " + std::to_string(WSAGetLastError()), LOG_LEVEL::ERR);
            return false;
        }

        int status = connect(mSocket, (SOCKADDR*)& target, sizeof(target));
        if (status == SOCKET_ERROR) {
            screenPrinter->print("Socket connect failed with error: " + std::to_string(WSAGetLastError()), LOG_LEVEL::ERR);
            screenPrinter->print("Error connecting to WSPRNet", LOG_LEVEL::ERR);

            return false;
        }

        screenPrinter->debug("WSPRNet socket connection established");
        return true;
    }

    void sendReportWrapper(const wspr::Report& report) {
        const bool status = sendReport(report);
        if (status) {
            mCountSendsOK++;
        }
        else {
            mCountSendsErrored++;
            screenPrinter->err("Failed to send WSPR report to WSPRNet");
        }
    }

    bool sendReport(const wspr::Report& report) {

        std::ostringstream all;
        std::ostringstream content;
        std::ostringstream header;
        std::ostringstream request;

        content << "function=" << "wspr" << "&";
        content << "rcall=" << report.reporterCallsign << "&";
        content << "rgrid=" << operatorGrid << "&";

        // frequency in MHz
        const float rfreqf = static_cast<float>(report.recvfreq) / static_cast<float>(1000000.0);
        content << "rqrg=" << std::fixed << std::setprecision(6) << rfreqf << "&";

        // date, yymmdd utc
        content << "date=";
        std::chrono::system_clock::time_point tp{ std::chrono::seconds{report.epochTime} };
        time_t tt = std::chrono::system_clock::to_time_t(tp);
        tm utc = *gmtime(&tt);

        content << std::setfill('0') << std::setw(2) << utc.tm_year - 100;
        content << std::setfill('0') << std::setw(2) << utc.tm_mon + 1; // tm_mon is 0-based
        content << std::setfill('0') << std::setw(2) << utc.tm_mday;
        content << "&";

        // time, hhmm utc
        content << "time=";
        content << std::setfill('0') << std::setw(2) << utc.tm_hour;
        content << std::setfill('0') << std::setw(2) << utc.tm_min;
        content << "&";

        // snr
        content << "sig=" << report.snr << "&";

        // dt
        content << "dt=" << std::setprecision(2) << report.dt << "&";

        // drift
        content << "drift=" << report.drift << "&";

        content << "tcall=" << report.callsign << "&";

        content << "tgrid=" << report.locator << "&";

        const float freqf = static_cast<float>(report.freq) / static_cast<float>(1000000.0);
        content << "tqrg=" << std::fixed << std::setprecision(6) << freqf << "&";

        content << "dbm=" << report.dbm << "&";

        content << "version=" << PROGRAM_NAME << " " << PROGRAM_VERSION << "&";

        content << "mode=2";

        // http request line
        request << "POST /post? HTTP/1.1\r\n";

        // header
        header << "Connection: Keep-Alive\r\n";
        header << "Host: wsprnet.org\r\n";
        header << "Content-Type: application/x-www-form-urlencoded\r\n";
        content.seekp(0, ios::end);
        const auto contentLength = content.tellp();
        content.seekp(0, ios::beg);
        screenPrinter->debug("content length: " + std::to_string(contentLength));
        header << "Content-Length: " << std::to_string(contentLength) << "\r\n";
        header << "Accept-Language: en-US,*\r\n";
        header << "User-Agent: Mozilla/5.0\r\n";

        all << header.str();
        all << "\r\n"; // blank line between headers and body
        all << content.str();

        bool sendSuccess = sendMessageWithRetry(request.str());
        if (!sendSuccess) {
            screenPrinter->debug("Failed to send data to WSPRNet");
            return false;
        }

        sendSuccess = sendMessageWithRetry(all.str());
        if (!sendSuccess) {
            screenPrinter->debug("Failed to send data to WSPRNet");
            return false;
        }

        int numTries= 0;
        std::string response = "";
        do {
            if (numTries) { 
                std::this_thread::sleep_for(std::chrono::milliseconds(333));
            }
            numTries++;
            screenPrinter->debug("WSPRNet attempting read, try: " + std::to_string(numTries));
            response += readMessage();
            screenPrinter->debug("WSPRNet read message of size: " + std::to_string(response.size()) + "message: " + response);
        }
        while (response.empty() && numTries <= 3);

        if (response.size()) {
            screenPrinter->debug("WSPRNet received response: " + response);
        }
        else {
            screenPrinter->debug("WSPRNet No response received, giving up!");
      //      closeSocket();
            return false;
        }

    //    closeSocket();
        return true;
    }

    bool sendMessageWithRetry(const std::string& message) {

        int tries = 0;
        bool sendSuccess = false;
        while (tries <= 2) {
            ++tries;
            screenPrinter->debug("Sending message: " + message);
            const int bytesSent = sendMessage(message);
            screenPrinter->debug("sent bytes: " + std::to_string(bytesSent)  +" message size: " + std::to_string(message.size()));
            if (bytesSent == message.size()) {
                screenPrinter->debug("message send success!");
                sendSuccess = true;
                break; // success!
            }
            else if (bytesSent == SOCKET_ERROR) {
                return false;
            }
        }
        return sendSuccess;
    }

    int sendMessage(const std::string& message) {
        int total = static_cast<int>(message.size());
        int sent = 0;
        do {
            int bytes = send(mSocket, message.c_str() + sent, total - sent, NULL);
            screenPrinter->debug("send() call resulted in value: " + std::to_string(bytes));
            if (bytes == SOCKET_ERROR) {
                return SOCKET_ERROR;
            }
            sent += bytes;
            if (sent < total) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } while (sent < total);

        return sent;
    }

    std::string readMessage() {
        int received = 0;
        std::string message = "";
        int bytes = 0;
        char buf[8192] = {0};
        bytes = recv(mSocket, buf, 8192, NULL);
        screenPrinter->debug("recv() call yielded " + std::to_string(bytes) + " bytes");
        if (bytes > 0) {
            message.append(buf);
        }
        return message;
    }

    void processingLoop() {
        while (!terminateFlag) {
            screenPrinter->debug("Reports in send queue: " + std::to_string(mReports.size()));

            while (!mReports.empty()) {
                const bool connectStatus = connectSocket();
                if (!connectStatus) { continue; }
                auto report = mReports.dequeue();
                sendReportWrapper(report);
                closeSocket();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10000));
            if (terminateFlag) { return; }

            reportStats();
        }
    }

    void reportStats() {
        screenPrinter->debug("Count of successful reports to WSPRNet: " + std::to_string(mCountSendsOK));
        screenPrinter->debug("Count of errored reports to WSPRNet: " + std::to_string(mCountSendsErrored));
    }

    std::shared_ptr<ScreenPrinter> screenPrinter;
    SafeQueue< wspr::Report > mReports;
    const std::string SERVER_IP = "50.235.87.130";
    const int HTTP_PORT = 80;
    bool terminateFlag;
    std::thread mSendThread;
    SOCKET mSocket;
    SOCKADDR_IN target;
    std::string operatorGrid;

    std::atomic_int mCountSendsOK;
    std::atomic_int mCountSendsErrored;
};
