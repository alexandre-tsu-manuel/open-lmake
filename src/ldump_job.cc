// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "app.hh"
#include "disk.hh"
#include "rpc_job.hh"

using namespace Disk ;

template<class V> void _print_map(::vmap_s<V> const& m) {
	size_t w = 0 ;
	for( auto const& [k,v] : m ) w = ::max(w,k.size()) ;
	for( auto const& [k,v] : m ) ::cout <<'\t'<< ::setw(w)<<k <<" : "<< v <<'\n' ;
}

void _print_views(::vmap_s<JobSpace::ViewDescr> const& m) {
	size_t w = 0 ;
	for( auto const& [k,v] : m ) w = ::max(w,k.size()) ;
	for( auto const& [k,v] : m ) ::cout <<'\t'<< ::setw(w)<<k <<" : "<< v.phys <<' '<< v.copy_up <<'\n' ;
}

void print_submit_attrs(SubmitAttrs const& sa) {
	::cout << "--submit attrs--\n" ;
	//
	::cout << "backend  : "  << snake(sa.tag)           <<'\n' ;
	::cout << "pressure : "  << sa.pressure.short_str() <<'\n' ;
	::cout << "live_out : "  << sa.live_out             <<'\n' ;
	::cout << "reason   : "  << sa.reason               <<'\n' ;
}

void print_pre_start(JobRpcReq const& jrr) {
	SWEAR( jrr.proc==JobRpcProc::Start , jrr.proc ) ;
	::cout << "--req--\n" ;
	//
	::cout << "seq_id : " << jrr.seq_id <<'\n' ;
	::cout << "job    : " << jrr.job    <<'\n' ;
	//
	::cout << "backend_msg :\n" ; ::cout << ensure_nl(indent(jrr.msg)) ;
}

void print_start(JobRpcReply const& jrr) {
	SWEAR( jrr.proc==JobRpcProc::Start , jrr.proc ) ;
	::cout << "--start--\n" ;
	//
	::cout << "addr         : "  << hex<<jrr.addr<<dec          <<'\n' ;
	::cout << "auto_mkdir   : "  << jrr.autodep_env.auto_mkdir  <<'\n' ;
	::cout << "chroot_dir_s : "  << jrr.job_space.chroot_dir_s  <<'\n' ;
	::cout << "cwd_s        : "  << jrr.cwd_s                   <<'\n' ;
	::cout << "date_prec    : "  << jrr.date_prec               <<'\n' ;
	::cout << "ignore_stat  : "  << jrr.autodep_env.ignore_stat <<'\n' ;
	::cout << "interpreter  : "  << jrr.interpreter             <<'\n' ;
	::cout << "keep_tmp     : "  << jrr.keep_tmp                <<'\n' ;
	::cout << "key          : "  << jrr.key                     <<'\n' ;
	::cout << "kill_sigs    : "  << jrr.kill_sigs               <<'\n' ;
	::cout << "live_out     : "  << jrr.live_out                <<'\n' ;
	::cout << "method       : "  << jrr.method                  <<'\n' ;
	::cout << "tmp_dir_s    : "  << jrr.autodep_env.tmp_dir_s   <<'\n' ; // tmp directory on disk
	::cout << "root_view_s  : "  << jrr.job_space.root_view_s   <<'\n' ;
	::cout << "small_id     : "  << jrr.small_id                <<'\n' ;
	::cout << "stdin        : "  << jrr.stdin                   <<'\n' ;
	::cout << "stdout       : "  << jrr.stdout                  <<'\n' ;
	::cout << "timeout      : "  << jrr.timeout                 <<'\n' ;
	::cout << "tmp_sz_mb    : "  << jrr.tmp_sz_mb               <<'\n' ;
	::cout << "tmp_view_s   : "  << jrr.job_space.tmp_view_s    <<'\n' ;
	::cout << "use_script   : "  << jrr.use_script              <<'\n' ;
	//
	::cout << "deps :\n"           ; _print_map  (jrr.deps           )                         ;
	::cout << "env :\n"            ; _print_map  (jrr.env            )                         ;
	::cout << "star matches :\n"   ; _print_map  (jrr.star_matches   )                         ;
	::cout << "static matches :\n" ; _print_map  (jrr.static_matches )                         ;
	::cout << "views :\n"          ; _print_views(jrr.job_space.views)                         ;
	::cout << "cmd :\n"            ; ::cout << ensure_nl(indent(jrr.cmd.first+jrr.cmd.second)) ;
}

void print_end(JobRpcReq const& jrr) {
	JobDigest const& jd = jrr.digest ;
	JobStats  const& st = jd.stats   ;
	SWEAR( jrr.proc==JobRpcProc::End , jrr.proc ) ;
	//
	::cout << "--end--\n" ;
	//
	::cout << "phy_dynamic_tmp_s  : " << jrr.phy_tmp_dir_s <<'\n' ;
	//
	::cout << "digest.status      : " << jd.status         <<'\n' ;
	::cout << "digest.wstatus     : " << jd.wstatus        <<'\n' ;
	::cout << "digest.end_date    : " << jd.end_date       <<'\n' ;
	::cout << "digest.stats.cpu   : " << st.cpu            <<'\n' ;
	::cout << "digest.stats.job   : " << st.job            <<'\n' ;
	::cout << "digest.stats.total : " << st.total          <<'\n' ;
	::cout << "digest.stats.mem   : " << st.mem            <<'\n' ;
	//
	::cout << "dynamic_env :\n"         ; _print_map(jrr.dynamic_env)            ;
	//
	::cout << "digest.targets :\n"      ; _print_map(jd.targets     )            ;
	::cout << "digest.deps :\n"         ; _print_map(jd.deps        )            ;
	::cout << "digest.stderr :\n"       ; ::cout << ensure_nl(indent(jd.stderr)) ;
	::cout << "digest.stdout :\n"       ; ::cout << ensure_nl(indent(jd.stdout)) ;
	//
	::cout << "_msg :\n" ; ::cout << ensure_nl(indent(localize(jrr.msg))) ;
}

int main( int argc , char* argv[] ) {
	if (argc!=2) exit(Rc::Usage,"usage : ldump_job file") ;
	app_init(true/*read_only_ok*/) ;
	//
	JobInfo job_info { argv[1] } ;
	if (+job_info.start) {
		::cout << "eta  : " << job_info.start.eta                  <<'\n' ;
		::cout << "host : " << SockFd::s_host(job_info.start.host) <<'\n' ;
		print_submit_attrs(job_info.start.submit_attrs) ;
		::cout << "rsrcs :\n" ; _print_map(job_info.start.rsrcs) ;
		print_pre_start   (job_info.start.pre_start   ) ;
		print_start       (job_info.start.start       ) ;
	}
	//
	if (+job_info.end) print_end(job_info.end.end) ;
	return 0 ;
}
