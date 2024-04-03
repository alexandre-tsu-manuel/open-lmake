// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "ld_server.hh"

thread_local bool                      AutodepLock::t_active = false ;
/**/         Mutex<MutexLvl::Autodep1> AutodepLock::_s_mutex ;

static bool started() { return AutodepLock::t_active ; } // no auto-start for server

#define IN_SERVER
#include "ld.x.cc"
