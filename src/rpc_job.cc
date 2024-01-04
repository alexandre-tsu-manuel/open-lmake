// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "disk.hh"
#include "hash.hh"

#include "rpc_job.hh"

using namespace Disk ;
using namespace Hash ;

//
// FileAction
//

::ostream& operator<<( ::ostream& os , FileAction const& fa ) {
	/**/                                                  os << "FileAction(" << fa.tag ;
	if ( fa.tag<=FileActionTag::HasFile                 ) os <<','<< fa.date            ;
	if ( fa.tag<=FileActionTag::HasFile && fa.manual_ok ) os <<",manual_ok"             ;
	return                                                os <<')'                      ;
}

::pair<vector_s/*unlinks*/,pair_s<bool/*ok*/>> do_file_actions( ::vmap_s<FileAction> const& pre_actions , NfsGuard& nfs_guard , Hash::Algo ha ) {
	::uset_s   keep_dirs ;
	::vector_s unlinks   ;
	::string   msg       ;
	bool       ok        = true ;
	//
	auto keep = [&](::string const& dir)->void {
		for( ::string d=dir ; +d ; d = dir_name(d) ) if (!keep_dirs.insert(d).second) break ;
	} ;
	//
	for( auto const& [f,a] : pre_actions ) {                                                             // pre_actions are adequately sorted
		{ if ( a.tag>FileActionTag::HasFile     ) continue ; } FileInfo fi = FileInfo(nfs_guard.access(f)) ;
		{ if ( !fi || fi.date==a.date           ) continue ; } append_to_string( msg , "manual " , mk_file(f) ) ;
		{ if ( a.manual_ok                      ) continue ; }
		{ if ( +a.crc && a.crc.match(Crc(f,ha)) ) continue ; } ok = false ;
	}
	if (ok)
		for( auto const& [f,a] : pre_actions ) {                                   // pre_actions are adequately sorted
			SWEAR(+f) ;                                                            // acting on root dir is non-sense
			switch (a.tag) {
				case FileActionTag::Keep     :                                                               break ;
				case FileActionTag::Unlink   : nfs_guard.change(f) ; if (unlink  (f)) unlinks.push_back(f) ; break ;
				case FileActionTag::Uniquify : nfs_guard.change(f) ; if (uniquify(f)) keep(dir_name(f))    ; break ;
				case FileActionTag::Mkdir :
					if (!keep_dirs.contains(f)) {
						mkdir(f,nfs_guard) ;
						keep(f) ;
					}
				break ;
				case FileActionTag::Rmdir :
					for( ::string d=f ; +d && !keep_dirs.contains(d) ; d=dir_name(d) ) {
						nfs_guard.change(f) ;
						try                     { rmdir(f) ; }
						catch (::string const&) { keep (f) ; }
					}
				break ;
				default : FAIL(a) ;
			}
		}
	return {unlinks,{msg,ok}} ;
}

//
// JobReason
//

::ostream& operator<<( ::ostream& os , JobReason const& jr ) {
	os << "JobReason(" << jr.tag ;
	if (jr.tag>=JobReasonTag::HasNode) os << ',' << jr.node ;
	return os << ')' ;
}

//
// SubmitAttrs
//

::ostream& operator<<( ::ostream& os , SubmitAttrs const& sa ) {
	/**/                             os << "SubmitAttrs("  ;
	if (sa.tag!=BackendTag::Unknown) os << sa.tag    <<',' ;
	if (sa.live_out                ) os << "live_out,"     ;
	return                           os << sa.reason <<')' ;
}

//
// JobRpcReq
//

::ostream& operator<<( ::ostream& os , TargetDigest const& td ) {
	const char* sep = "" ;
	/**/                os << "TargetDigest("  ;
	if (+td.accesses) { os <<sep<< td.accesses ; sep = "," ; }
	if ( td.write   ) { os <<sep<< "write"     ; sep = "," ; }
	if (+td.tflags  ) { os <<sep<< td.tflags   ; sep = "," ; }
	if (+td.crc     ) { os <<sep<< td.crc      ; sep = "," ; }
	return              os <<')'               ;
}

::ostream& operator<<( ::ostream& os , JobDigest const& jd ) {
	return os << "JobDigest(" << jd.wstatus<<':'<<jd.status <<','<< jd.targets <<','<< jd.deps << ')' ;
}

::ostream& operator<<( ::ostream& os , JobRpcReq const& jrr ) {
	os << "JobRpcReq(" << jrr.proc <<','<< jrr.seq_id <<','<< jrr.job ;
	switch (jrr.proc) {
		case JobProc::LiveOut  : os <<','<< jrr.msg         ; break ;
		case JobProc::DepInfos : os <<','<< jrr.digest.deps ; break ;
		case JobProc::End      : os <<','<< jrr.digest      ; break ;
		default                :                              break ;
	}
	return os << ')' ;
}

JobRpcReq::JobRpcReq( SI si , JI j , JobExecRpcReq&& jerr ) : seq_id{si} , job{j} {
	switch (jerr.proc) {
		case JobExecRpcProc::Decode : proc = P::Decode  ; msg = ::move(jerr.txt) ; file = ::move(jerr.files[0].first) ; ctx = ::move(jerr.ctx) ;                          break ;
		case JobExecRpcProc::Encode : proc = P::Encode  ; msg = ::move(jerr.txt) ; file = ::move(jerr.files[0].first) ; ctx = ::move(jerr.ctx) ; min_len = jerr.min_len ; break ;
		case JobExecRpcProc::DepInfos : {
			::vmap_s<DepDigest> ds ; ds.reserve(jerr.files.size()) ;
			for( auto&& [dep,date] : jerr.files ) ds.emplace_back( ::move(dep) , DepDigest(jerr.digest.accesses,jerr.digest.dflags,true/*parallel*/,date) ) ;
			proc        = P::DepInfos ;
			digest.deps = ::move(ds) ;
		} break ;
		default : FAIL(jerr.proc) ;
	}
}

//
// JobRpcReply
//

::ostream& operator<<( ::ostream& os , TargetSpec const& ts ) {
	return os << "TargetSpec(" << ts.pattern <<','<< ts.tflags <<')' ;
}

::ostream& operator<<( ::ostream& os , JobRpcReply const& jrr ) {
	os << "JobRpcReply(" << jrr.proc ;
	switch (jrr.proc) {
		case JobProc::ChkDeps  : os <<','<< jrr.ok        ; break ;
		case JobProc::DepInfos : os <<','<< jrr.dep_infos ; break ;
		case JobProc::Start :
			/**/                  os <<',' << hex<<jrr.addr<<dec               ;
			/**/                  os <<',' << jrr.autodep_env                  ;
			if (+jrr.chroot     ) os <<',' << jrr.chroot                       ;
			if (+jrr.cwd_s      ) os <<',' << jrr.cwd_s                        ;
			/**/                  os <<',' << mk_printable(to_string(jrr.env)) ; // env may contain the non-printable EnvPassMrkr value
			if (+jrr.static_deps) os <<',' << jrr.static_deps                  ;
			/**/                  os <<',' << jrr.interpreter                  ;
			if (jrr.keep_tmp    ) os <<',' << "keep_tmp"                       ;
			/**/                  os <<',' << jrr.kill_sigs                    ;
			if (jrr.live_out    ) os <<',' << "live_out"                       ;
			/**/                  os <<',' << jrr.method                       ;
			/**/                  os <<',' << jrr.remote_admin_dir             ;
			/**/                  os <<',' << jrr.small_id                     ;
			if (+jrr.stdin      ) os <<'<' << jrr.stdin                        ;
			if (+jrr.stdout     ) os <<'>' << jrr.stdout                       ;
			/**/                  os <<"*>"<< jrr.targets                      ;
			if (+jrr.timeout    ) os <<',' << jrr.timeout                      ;
			/**/                  os <<',' << jrr.cmd                          ; // last as it is most probably multi-line
			;
		break ;
		default : ;
	}
	return os << ')' ;
}

//
// JobExecRpcReq
//

::ostream& operator<<( ::ostream& os , JobExecRpcReq::AccessDigest const& ad ) {
	const char* sep = "" ;
	/**/                                 os << "AccessDigest("                                      ;
	if (+ad.accesses                 ) { os <<sep     << ad.accesses                                ; sep = "," ; }
	if (+ad.dflags                   ) { os <<sep     << ad.dflags                                  ; sep = "," ; }
	if ( ad.prev_write  || ad.write  ) { os <<sep     << "write:"<<ad.prev_write<<"->"<<ad.write    ; sep = "," ; }
	if (+ad.neg_tflags               ) { os <<sep<<'-'<< ad.neg_tflags                              ; sep = "," ; }
	if (+ad.pos_tflags               ) { os <<sep<<'+'<< ad.pos_tflags                              ; sep = "," ; }
	if ( ad.prev_unlink || ad.unlink ) { os <<sep     << "unlink:"<<ad.prev_unlink<<"->"<<ad.unlink ; sep = "," ; }
	return                               os <<')'                                                   ;
}

void JobExecRpcReq::AccessDigest::update( AccessDigest const& ad , AccessOrder order ) {
	dflags |= ad.dflags ;                                                      // in all cases, dflags are always accumulated
	if ( order<AccessOrder::Write || idle() ) {
		if ( order==AccessOrder::Before && !ad.idle() ) accesses  = Accesses::None ;
		/**/                                            accesses |= ad.accesses    ;
	}
	if (order>=AccessOrder::Write) {
		neg_tflags &= ~ad.pos_tflags ; neg_tflags |= ad.neg_tflags ;           // ad flags have priority over this flags
		pos_tflags &= ~ad.neg_tflags ; pos_tflags |= ad.pos_tflags ;           // .
	} else {
		neg_tflags |= ad.neg_tflags & ~pos_tflags ;                            // this flags have priority over ad flags
		pos_tflags |= ad.pos_tflags & ~neg_tflags ;                            // .
	}
	if (!ad.idle()) {
		if ( idle() || order==AccessOrder::After ) { prev_unlink = unlink ; unlink &= !ad.write ; unlink |= ad.unlink ; }
		/**/                                       { prev_write  = write  ;                       write  |=  ad.write ; }
	}
}

::ostream& operator<<( ::ostream& os , JobExecRpcReq const& jerr ) {
	/**/                os << "JobExecRpcReq(" << jerr.proc <<','<< jerr.date ;
	if (jerr.sync     ) os << ",sync"                                         ;
	if (jerr.auto_date) os << ",auto_date"                                    ;
	if (jerr.no_follow) os << ",no_follow"                                    ;
	/**/                os <<',' << jerr.digest                               ;
	if (+jerr.txt     ) os <<',' << jerr.txt                                  ;
	if (jerr.proc>=JobExecRpcProc::HasFile) {
		if ( +jerr.digest.accesses && !jerr.auto_date ) {
			os <<','<< jerr.files ;
		} else {
			::vector_s fs ;
			for( auto [f,d] : jerr.files ) fs.push_back(f) ;
			os <<','<< fs ;
		}
	}
	return os <<')' ;
}

//
// JobExecRpcReply
//

::ostream& operator<<( ::ostream& os , JobExecRpcReply const& jerr ) {
	os << "JobExecRpcReply(" << jerr.proc ;
	switch (jerr.proc) {
		case JobExecRpcProc::ChkDeps  : os <<','<< jerr.ok        ; break ;
		case JobExecRpcProc::DepInfos : os <<','<< jerr.dep_infos ; break ;
		default : ;
	}
	return os << ')' ;
}

JobExecRpcReply::JobExecRpcReply( JobRpcReply const& jrr ) {
	switch (jrr.proc) {
		case JobProc::None     :                        proc = Proc::None     ;                             break ;
		case JobProc::ChkDeps  : SWEAR(jrr.ok!=Maybe) ; proc = Proc::ChkDeps  ; ok        = jrr.ok        ; break ;
		case JobProc::DepInfos :                        proc = Proc::DepInfos ; dep_infos = jrr.dep_infos ; break ;
		default : FAIL(jrr.proc) ;
	}
}

//
// JobSserverRpcReq
//

::ostream& operator<<( ::ostream& os , JobServerRpcReq const& jsrr ) {
	/**/                                        os << "JobServerRpcReq(" << jsrr.proc <<','<< jsrr.seq_id ;
	if (jsrr.proc==JobServerRpcProc::Heartbeat) os <<','<< jsrr.job                                       ;
	return                                      os <<')'                                                  ;
}

//
// JobInfoStart
//

::ostream& operator<<( ::ostream& os , JobInfoStart const& jis ) {
	return os << "JobInfoStart(" << jis.submit_attrs <<','<< jis.rsrcs <<','<< jis.pre_start <<','<< jis.start <<')' ;
}

//
// JobInfoEnd
//

::ostream& operator<<( ::ostream& os , JobInfoEnd const& jie ) {
	return os << "JobInfoEnd(" << jie.end <<')' ;
}
