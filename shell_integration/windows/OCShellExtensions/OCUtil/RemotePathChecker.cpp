/**
* Copyright (c) 2014 ownCloud, Inc. All rights reserved.
*
* This library is free software; you can redistribute it and/or modify it under
* the terms of the GNU Lesser General Public License as published by the Free
* Software Foundation; version 2.1 of the License
*
* This library is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
* FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
* details.
*/

#include "CommunicationSocket.h"

#include "RemotePathChecker.h"
#include "StringUtil.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <iterator>
#include <unordered_set>
#include <cassert>

#include <shlobj.h>

using namespace std;


// This code is run in a thread
void RemotePathChecker::workerThreadLoop()
{
    CommunicationSocket socket;
    std::unordered_set<std::wstring> asked;
    if (!socket.Connect()) {
        return;
        //FIXME! what if this fails!  what if we are disconnected later?
    }

    while(!_stop) {
        {
            std::unique_lock<std::mutex> lock(_mutex);
            while (!_pending.empty() && !_stop) {
                auto filePath = _pending.front();
                _pending.pop();

                lock.unlock();
                if (!asked.count(filePath)) {
                    asked.insert(filePath);
                    socket.SendMsg(wstring(L"RETRIEVE_FILE_STATUS:" + filePath + L'\n').data());
                }
                lock.lock();
            }
        }

        std::wstring response;
        while (!_stop && socket.ReadLine(&response)) {
            if (StringUtil::begins_with(response, wstring(L"REGISTER_PATH:"))) {
                wstring responsePath = response.substr(14); // length of REGISTER_PATH:

                std::unique_lock<std::mutex> lock(_mutex);
                _watchedDirectories.push_back(responsePath);
            } else if (StringUtil::begins_with(response, wstring(L"STATUS:")) ||
                    StringUtil::begins_with(response, wstring(L"BROADCAST:"))) {

                auto statusBegin = response.find(L':', 0);
                assert(statusBegin != std::wstring::npos);

                auto statusEnd = response.find(L':', statusBegin + 1);
                if (statusEnd == std::wstring::npos) {
                    // the command do not contains two colon?
                    continue;
                }

                auto responseStatus = response.substr(statusBegin+1, statusEnd - statusBegin-1);
                auto responsePath = response.substr(statusEnd+1);
                auto state = _StrToFileState(responseStatus);
                auto erased = asked.erase(responsePath);

                {   std::unique_lock<std::mutex> lock(_mutex);
                    _cache[responsePath] = state;
                }
                SHChangeNotify(SHCNE_MKDIR, SHCNF_PATH, responsePath.data(), NULL);
            }
        }

        if (_stop)
            return;
    }
}



RemotePathChecker::RemotePathChecker()
    : _thread([this]{ this->workerThreadLoop(); } )
    , _newQueries(CreateEvent(NULL, true, true, NULL))
{
}

RemotePathChecker::~RemotePathChecker()
{
    _stop = true;
    //_newQueries.notify_all();
    SetEvent(_newQueries);
    _thread.join();
    CloseHandle(_newQueries);
}

vector<wstring> RemotePathChecker::WatchedDirectories()
{
    std::unique_lock<std::mutex> lock(_mutex);
    return _watchedDirectories;
}

bool RemotePathChecker::IsMonitoredPath(const wchar_t* filePath, int* state)
{
    assert(state); assert(filePath);

    std::unique_lock<std::mutex> lock(_mutex);

    auto path = std::wstring(filePath);

    auto it = _cache.find(path);
    if (it != _cache.end()) {
        *state = it->second;
        return true;
    }

    _pending.push(filePath);
    SetEvent(_newQueries);
    return false;

}

RemotePathChecker::FileState RemotePathChecker::_StrToFileState(const std::wstring &str)
{
	if (str == L"NOP" || str == L"NONE") {
		return StateNone;
	} else if (str == L"SYNC" || str == L"NEW") {
		return StateSync;
	} else if (str == L"SYNC+SWM" || str == L"NEW+SWM") {
		return StateSyncSWM;
	} else if (str == L"OK") {
		return StateOk;
	} else if (str == L"OK+SWM") {
		return StateOkSWM;
	} else if (str == L"IGNORE") {
		return StateWarning;
	} else if (str == L"IGNORE+SWM") {
		return StateWarningSWM;
	} else if (str == L"ERROR") {
		return StateError;
	} else if (str == L"ERROR+SWM") {
		return StateErrorSWM;
	}

	return StateNone;
}