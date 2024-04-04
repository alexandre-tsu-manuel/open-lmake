// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "app.hh"
#include "client.hh"
#include "disk.hh"
#include "process.hh"
#include "rpc_client.hh"
#include "thread.hh"
#include "trace.hh"

using namespace Disk ;
using namespace Time ;

::atomic<bool> g_seen_int = false ;

static void _int_thread_func( ::stop_token stop , Fd int_fd ) {
	t_thread_key = 'I' ;
	Trace trace ;
	::stop_callback stop_cb { stop , [&](){ kill_self(SIGINT) ; } } ;          // transform request_stop into an event we wait for
	trace("start") ;
	for(;;) {
		struct signalfd_siginfo _ ;
		ssize_t cnt = ::read(int_fd,&_,sizeof(_)) ;
		SWEAR( cnt==sizeof(_) , cnt ) ;
		if (stop.stop_requested()) {
			trace("done") ;
			return ;                                                           // not an interrupt, just normal exit
		}
		trace("send_int") ;
		OMsgBuf().send(g_server_fds.out,ReqRpcReq(ReqProc::Kill)) ;
		::cout << ::endl ;                                                     // output is nicer if ^C is on its own line
		g_seen_int = true ;
	}
}

static void _handle_int() {
	struct Exit {
		Exit() : int_fd{open_sig_fd({SIGINT})} {}
		~Exit() {                                                              // this must be executed after _int_thread_func has completed
			if (!g_seen_int) return ;
			close_sig_fd(int_fd,{SIGINT}) ;
			kill_self(SIGINT) ;                                                // appear as being interrupted : important for shell scripts to actually stop
			kill_self(SIGHUP) ;                                                // for some reason, the above kill_self does not work in some situations (e.g. if you type bash -c 'lmake&')
			fail_prod("lmake does not want to die") ;
		}
		Fd int_fd ;
	} ;
	static Exit      exit   ;
	static ::jthread int_jt { _int_thread_func , exit.int_fd } ;
}

int main( int argc , char* argv[] ) {
	Trace::s_backup_trace = true ;
	app_init(Maybe/*chk_version*/) ;
	//
	ReqSyntax syntax{{
		{ ReqFlag::Archive         , { .short_name='a' , .has_arg=false , .doc="ensure all intermediate files are generated" } }
	,	{ ReqFlag::ForgetOldErrors , { .short_name='e' , .has_arg=false , .doc="assume old errors are transcient"            } }
	,	{ ReqFlag::Jobs            , { .short_name='j' , .has_arg=true  , .doc="max number of jobs"                          } }
	,	{ ReqFlag::Local           , { .short_name='l' , .has_arg=false , .doc="launch all jobs locally"                     } }
	,	{ ReqFlag::LiveOut         , { .short_name='o' , .has_arg=false , .doc="generate live output for last job"           } }
	,	{ ReqFlag::SourceOk        , { .short_name='s' , .has_arg=false , .doc="allow overwrite of source files"             } }
	,	{ ReqFlag::KeepTmp         , { .short_name='t' , .has_arg=false , .doc="keep tmp dir after job execution"            } }
	,	{ ReqFlag::Verbose         , { .short_name='v' , .has_arg=false , .doc="generate backend execution info"             } }
	,	{ ReqFlag::Backend         , { .short_name='b' , .has_arg=true  , .doc="send arguments to backend"                   } }
	}} ;
	// add args passed in environment
	SWEAR(argc>0) ;
	::vector_s            env_args = split(get_env("LMAKE_ARGS"))                   ;
	::vector<const char*> args     = {argv[0]} ; args.reserve(env_args.size()+argc) ;
	for( ::string const& a : env_args ) args.push_back(a.c_str()) ;
	for( int i=1 ; i<argc ; i++       ) args.push_back(argv[i]  ) ;
	Trace trace("main",::c_vector_view<const char*>(argv,argc)) ;
	/**/  trace("main",env_args                               ) ;
	/**/  trace("main",args                                   ) ;
	//
	ReqCmdLine      cmd_line { syntax , int(args.size()) , args.data() } ;
	::string const& n_jobs   = cmd_line.flag_args[+ReqFlag::Jobs]        ;
	try                       { from_string<JobIdx>(n_jobs,true/*empty_ok*/) ;                                      }
	catch (::string const& e) { syntax.usage(to_string("cannot understand max number of jobs (",e,") : ",n_jobs)) ; }
	// start interrupt handling thread once server is started
	//vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
	Bool3 ok = out_proc( ReqProc::Make , true/*refresh_makefiles*/ , syntax , cmd_line , _handle_int ) ;
	//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
	exit(mk_rc(ok)) ;
}
