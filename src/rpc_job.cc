// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include <sys/mount.h>

#include "disk.hh"
#include "hash.hh"
#include "trace.hh"

#include "rpc_job.hh"

#include "autodep/fuse.hh"

using namespace Disk ;
using namespace Hash ;

//
// FileAction
//

::ostream& operator<<( ::ostream& os , FileAction const& fa ) {
	/**/                                os << "FileAction(" << fa.tag ;
	if (fa.tag<=FileActionTag::HasFile) os <<','<< fa.sig             ;
	return                              os <<')'                      ;
}

::pair_s<bool/*ok*/> do_file_actions( ::vector_s* unlnks/*out*/ , ::vmap_s<FileAction>&& pre_actions , NfsGuard& nfs_guard ) {
	::uset_s keep_dirs ;
	::string msg       ;
	bool     ok        = true ;
	//
	Trace trace("do_file_actions",pre_actions) ;
	if (unlnks) unlnks->reserve(unlnks->size()+pre_actions.size()) ;                                       // most actions are unlinks
	for( auto const& [f,a] : pre_actions ) {                                                               // pre_actions are adequately sorted
		SWEAR(+f) ;                                                                                        // acting on root dir is non-sense
		switch (a.tag) {
			case FileActionTag::Unlink         :
			case FileActionTag::UnlinkWarning  :
			case FileActionTag::UnlinkPolluted :
			case FileActionTag::None           : {
				FileSig sig { nfs_guard.access(f) } ;
				if (!sig) break ;                                                                          // file does not exist, nothing to do
				bool done       = true/*garbage*/                                                        ;
				bool quarantine = sig!=a.sig && (a.crc==Crc::None||!a.crc.valid()||!a.crc.match(Crc(f))) ;
				if (quarantine) {
					done = ::rename( f.c_str() , dir_guard(QuarantineDirS+f).c_str() )==0 ;
					if (done) msg<<"quarantined "         <<mk_file(f)<<'\n' ;
					else      msg<<"failed to quarantine "<<mk_file(f)<<'\n' ;
				} else {
					SWEAR(is_lcl(f)) ;
					try {
						done = unlnk(nfs_guard.change(f)) ;
						if (a.tag==FileActionTag::None) { if ( done) msg << "unlinked "           << mk_file(f) << '\n' ; }
						else                            { if (!done) msg << "file disappeared : " << mk_file(f) << '\n' ; }
						done = true ;
					} catch (::string const& e) {
						msg <<  e << '\n' ;
						done = false ;
					}
				}
				trace(STR(quarantine),STR(done),f) ;
				if ( done && unlnks ) unlnks->push_back(f) ;
				ok &= done ;
			} break ;
			case FileActionTag::NoUniquify : if (can_uniquify(nfs_guard.change(f))) msg<<"did not uniquify "<<mk_file(f)<<'\n' ; break ;
			case FileActionTag::Uniquify   : if (uniquify    (nfs_guard.change(f))) msg<<"uniquified "      <<mk_file(f)<<'\n' ; break ;
			case FileActionTag::Mkdir      : mk_dir_s(with_slash(f),nfs_guard) ;                                                 break ;
			case FileActionTag::Rmdir      :
				if (!keep_dirs.contains(f))
					try {
						rmdir_s(with_slash(nfs_guard.change(f))) ;
					} catch (::string const&) {                                                            // if a dir cannot rmdir'ed, no need to try those uphill
						keep_dirs.insert(f) ;
						for( ::string d_s=dir_name_s(f) ; +d_s ; d_s=dir_name_s(d_s) )
							if (!keep_dirs.insert(no_slash(d_s)).second) break ;
					}
			break ;
		DF}
	}
	trace("done",STR(ok),localize(msg)) ;
	return {msg,ok} ;
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
// DepInfo
//

::ostream& operator<<( ::ostream& os , DepInfo const& di ) {
	switch (di.kind) {
		case DepInfoKind::Crc  : return os <<'('<< di.crc () <<')' ;
		case DepInfoKind::Sig  : return os <<'('<< di.sig () <<')' ;
		case DepInfoKind::Info : return os <<'('<< di.info() <<')' ;
	DF}
}

//
// JobSpace
//

::ostream& operator<<( ::ostream& os , JobSpace::ViewDescr const& vd ) {
	/**/             os <<"ViewDescr("<< vd.phys ;
	if (+vd.copy_up) os <<"CU:"<< vd.copy_up     ;
	return           os <<')'                    ;
}

::ostream& operator<<( ::ostream& os , JobSpace const& js ) {
	First first ;
	/**/                  os <<"JobSpace("                           ;
	if (+js.chroot_dir_s) os <<first("",",")<<"C:"<< js.chroot_dir_s ;
	if (+js.root_view_s ) os <<first("",",")<<"R:"<< js.root_view_s  ;
	if (+js.tmp_view_s  ) os <<first("",",")<<"T:"<< js.tmp_view_s   ;
	if (+js.views       ) os <<first("",",")<<"V:"<< js.views        ;
	return                os <<')'                                   ;
}

static void _chroot(::string const& dir_s) { Trace trace("_chroot",dir_s) ; if (::chroot(no_slash(dir_s).c_str())!=0) throw "cannot chroot to "+no_slash(dir_s)+" : "+strerror(errno) ; }
static void _chdir (::string const& dir_s) { Trace trace("_chdir" ,dir_s) ; if (::chdir (no_slash(dir_s).c_str())!=0) throw "cannot chdir to " +no_slash(dir_s)+" : "+strerror(errno) ; }

static void _mount_bind( ::string const& dst , ::string const& src ) { // src and dst may be files or dirs
	Trace trace("_mount_bind",dst,src) ;
	if (::mount( no_slash(src).c_str() , no_slash(dst).c_str() , nullptr/*type*/ , MS_BIND|MS_REC , nullptr/*data*/ )!=0)
		throw "cannot bind mount "+src+" onto "+dst+" : "+strerror(errno) ;
}

static vector<Fuse::Mount> _fuse_store ;
static void _mount_fuse( ::string const& dst_s , ::string const& src_s , ::string const& pfx_s , bool report_writes ) {
	Trace trace("_mount_fuse",dst_s,src_s,pfx_s,STR(report_writes)) ;
	_fuse_store.emplace_back( dst_s , src_s , pfx_s , report_writes ) ;
}

static void _mount_tmp( ::string const& dst_s , size_t sz_mb ) {
	SWEAR(sz_mb) ;
	Trace trace("_mount_tmp",dst_s,sz_mb) ;
	if (::mount( "" ,  no_slash(dst_s).c_str() , "tmpfs" , 0/*flags*/ , ("size="+::to_string(sz_mb)+"m").c_str() )!=0)
		throw "cannot mount tmpfs of size "+to_string_with_units<'M'>(sz_mb)+"B onto "+no_slash(dst_s)+" : "+strerror(errno) ;
}

static void _mount_overlay( ::string const& dst_s , ::vector_s const& srcs_s , ::string const& work_s ) {
	SWEAR(+srcs_s) ;
	SWEAR(srcs_s.size()>1,dst_s,srcs_s,work_s) ; // use bind mount in that case
	//
	Trace trace("_mount_overlay",dst_s,srcs_s,work_s) ;
	for( size_t i=1 ; i<srcs_s.size() ; i++ )
		if (srcs_s[i].find(':')!=Npos)
			throw "cannot overlay mount "+dst_s+" to "+fmt_string(srcs_s)+"with embedded columns (:)" ;
	mk_dir_s(work_s) ;
	//
	::string                                  data  = "userxattr"                      ;
	/**/                                      data += ",upperdir="+no_slash(srcs_s[0]) ;
	/**/                                      data += ",lowerdir="+no_slash(srcs_s[1]) ;
	for( size_t i=2 ; i<srcs_s.size() ; i++ ) data += ':'         +no_slash(srcs_s[i]) ;
	/**/                                      data += ",workdir=" +no_slash(work_s   ) ;
	if (::mount( nullptr ,  no_slash(dst_s).c_str() , "overlay" , 0 , data.c_str() )!=0)
		throw "cannot overlay mount "+dst_s+" to "+data+" : "+strerror(errno) ;
}

static void _atomic_write( ::string const& file , ::string const& data ) {
	Trace trace("_atomic_write",file,data) ;
	AutoCloseFd fd = ::open(file.c_str(),O_WRONLY|O_TRUNC) ;
	if (!fd) throw "cannot open "+file+" for writing" ;
	ssize_t cnt = ::write( fd , data.c_str() , data.size() ) ;
	if (cnt<0                  ) throw "cannot write atomically "s+data.size()+" bytes to "+file+" : "+strerror(errno)           ;
	if (size_t(cnt)<data.size()) throw "cannot write atomically "s+data.size()+" bytes to "+file+" : only "+cnt+" bytes written" ;
}

bool JobSpace::_is_lcl_tmp(::string const& f) const {
	if (is_lcl(f)  ) return true                      ;
	if (+tmp_view_s) return f.starts_with(tmp_view_s) ;
	/**/             return false                     ;
} ;

bool/*dst_ok*/ JobSpace::_create( ::vmap_s<MountAction>& deps , ::string const& dst , ::string const& src ) const {
	if (!_is_lcl_tmp(dst)) return false/*dst_ok*/ ;
	bool dst_ok = true ;
	if (is_dirname(dst)) {
		mk_dir_s(dst) ;
		deps.emplace_back(no_slash(dst),MountAction::Access) ;
	} else if (+FileInfo(dst).tag()) {
		deps.emplace_back(dst,MountAction::Access) ;
	} else if (+src) {
		/**/                        deps.emplace_back(src,MountAction::Read ) ;
		if ((dst_ok=+cpy(dst,src))) deps.emplace_back(dst,MountAction::Write) ;
		else                        dst_ok = false ;
	} else {
		AutoCloseFd fd = ::open(dir_guard(dst).c_str(),O_WRONLY|O_CREAT,0644) ;
		if ((dst_ok=+fd)) deps.emplace_back(dst,MountAction::Write) ;
	}
	return dst_ok ;
}

bool/*entered*/ JobSpace::enter(
	::vmap_s<MountAction>& report
,	::string const&        phy_root_dir_s
,	::string const&        phy_tmp_dir_s
,	size_t                 tmp_sz_mb
,	::string const&        work_dir_s
,	::vector_s const&      src_dirs_s
,	bool                   use_fuse
) {
	Trace trace("JobSpace::enter",*this,phy_root_dir_s,phy_tmp_dir_s,tmp_sz_mb,work_dir_s,src_dirs_s,STR(use_fuse)) ;
	//
	if ( !use_fuse && !*this ) return false/*entered*/ ;
	//
	int uid = ::getuid() ;          // must be done before unshare that invents a new user
	int gid = ::getgid() ;          // .
	//
	if (::unshare(CLONE_NEWUSER|CLONE_NEWNS)!=0) throw "cannot create namespace : "s+strerror(errno) ;
	//
	size_t   src_dirs_uphill_lvl = 0 ;
	::string highest             ;
	for( ::string const& d_s : src_dirs_s ) {
		if (!is_abs_s(d_s))
			if ( size_t ul=uphill_lvl_s(d_s) ; ul>src_dirs_uphill_lvl ) {
				src_dirs_uphill_lvl = ul  ;
				highest             = d_s ;
			}
	}
	//
	::string phy_super_root_dir_s ; // dir englobing all relative source dirs
	::string super_root_view_s    ; // .
	if (+root_view_s) {
		phy_super_root_dir_s = phy_root_dir_s ; for( size_t i=0 ; i<src_dirs_uphill_lvl ; i++ ) phy_super_root_dir_s = dir_name_s(phy_super_root_dir_s) ;
		super_root_view_s    = root_view_s    ; for( size_t i=0 ; i<src_dirs_uphill_lvl ; i++ ) super_root_view_s    = dir_name_s(super_root_view_s   ) ;
		SWEAR(phy_super_root_dir_s!="/",phy_root_dir_s,src_dirs_uphill_lvl) ;                                                                             // this should have been checked earlier
		if (!super_root_view_s) {
			highest.pop_back() ;
			throw
				"cannot map repository dir to "+no_slash(root_view_s)+" with relative source dir "+highest
			+	", "
			+	"consider setting <rule>.root_view="+mk_py_str("/repo"+phy_root_dir_s.substr(phy_super_root_dir_s.size()-1))
			;
		}
		if (root_view_s.substr(super_root_view_s.size())!=phy_root_dir_s.substr(phy_super_root_dir_s.size()))
			throw
				"last "s+src_dirs_uphill_lvl+" components do not match between physical root dir and root view"
			+	", "
			+	"consider setting <rule>.root_view="+mk_py_str("/repo/"+phy_root_dir_s.substr(phy_super_root_dir_s.size()))
			;
	}
	if ( +super_root_view_s && super_root_view_s.rfind('/',super_root_view_s.size()-2)!=0 ) throw "non top-level root_view not yet implemented"s ; // XXX : handle cases where dir is not top level
	if ( +tmp_view_s        && tmp_view_s       .rfind('/',tmp_view_s       .size()-2)!=0 ) throw "non top-level tmp_view not yet implemented"s  ; // .
	//
	::string chroot_dir       = chroot_dir_s                                                          ; if (+chroot_dir) chroot_dir.pop_back() ;
	bool     must_create_root = +super_root_view_s && !is_dir(chroot_dir+no_slash(super_root_view_s)) ;
	bool     must_create_tmp  = +tmp_view_s        && !is_dir(chroot_dir+no_slash(tmp_view_s       )) ;
	trace("create",STR(must_create_root),STR(must_create_tmp),STR(use_fuse)) ;
	if ( must_create_root || must_create_tmp || +views || use_fuse )
		try { unlnk_inside_s(work_dir_s) ; } catch (::string const& e) {} // if we need a work dir, we must clean it first as it is not cleaned upon exit (ignore errors as dir may not exist)
	if ( must_create_root || must_create_tmp || use_fuse ) {              // we cannot mount directly in chroot_dir
		if (!work_dir_s)
			throw
				"need a work dir to"s
			+	(	must_create_root ? " create root view"
				:	must_create_tmp  ? " create tmp view"
				:	use_fuse         ? " use fuse"
				:	                   " ???"
				)
			;
		::vector_s top_lvls        = lst_dir_s(+chroot_dir_s?chroot_dir_s:"/") ;
		::string   work_root_dir   = work_dir_s+"root"                         ;
		::string   work_root_dir_s = work_root_dir+'/'                         ;
		mk_dir_s      (work_root_dir_s) ;
		unlnk_inside_s(work_root_dir_s) ;
		trace("top_lvls",work_root_dir_s,top_lvls) ;
		for( ::string const& f : top_lvls ) {
			::string src_f     = (+chroot_dir_s?chroot_dir_s:"/"s) + f ;
			::string private_f = work_root_dir_s                   + f ;
			switch (FileInfo(src_f).tag()) {                                                                                   // exclude weird files
				case FileTag::Reg   :
				case FileTag::Empty :
				case FileTag::Exe   : OFStream{           private_f                 } ; _mount_bind(private_f,src_f) ; break ; // create file
				case FileTag::Dir   : mk_dir_s(with_slash(private_f)                ) ; _mount_bind(private_f,src_f) ; break ; // create dir
				case FileTag::Lnk   : lnk     (           private_f ,read_lnk(src_f)) ;                                break ; // copy symlink
			DN}
		}
		if (must_create_root) mk_dir_s(work_root_dir+super_root_view_s) ;
		if (must_create_tmp ) mk_dir_s(work_root_dir+tmp_view_s       ) ;
		chroot_dir = ::move(work_root_dir) ;
	}
	// mapping uid/gid is necessary to manage overlayfs
	_atomic_write( "/proc/self/setgroups" , "deny"                 ) ;                                                         // necessary to be allowed to write the gid_map (if desirable)
	_atomic_write( "/proc/self/uid_map"   , ""s+uid+' '+uid+" 1\n" ) ;
	_atomic_write( "/proc/self/gid_map"   , ""s+gid+' '+gid+" 1\n" ) ;
	//
	::string root_dir_s = +root_view_s ? root_view_s : phy_root_dir_s ;
	if (use_fuse) { //!                                                                                                                         pfx_s     report_writes
		/**/                                          _mount_fuse( chroot_dir+                 root_dir_s  ,                  phy_root_dir_s  , {}        , true      ) ;
		for( ::string const& src_dir_s : src_dirs_s ) _mount_fuse( chroot_dir+mk_abs(src_dir_s,root_dir_s) , mk_abs(src_dir_s,phy_root_dir_s) , src_dir_s , false     ) ;
	} else if (+root_view_s) {
		/**/                                          _mount_bind( chroot_dir+super_root_view_s            , phy_super_root_dir_s             ) ;
	}
	if (+tmp_view_s) {
		if      (+phy_tmp_dir_s) _mount_bind( chroot_dir+tmp_view_s , phy_tmp_dir_s ) ;
		else if (tmp_sz_mb     ) _mount_tmp ( chroot_dir+tmp_view_s , tmp_sz_mb     ) ;
	}
	//
	if      (+chroot_dir ) _chroot(chroot_dir)    ;
	if      (+root_view_s) _chdir(root_view_s   ) ;
	else if (+chroot_dir ) _chdir(phy_root_dir_s) ;
	//
	size_t work_idx = 0 ;
	for( auto const& [view,descr] : views ) if (+descr) {                                                                      // empty descr does not represent a view
		::string   abs_view = mk_abs(view,root_dir_s) ;
		::vector_s abs_phys ;                           abs_phys.reserve(descr.phys.size()) ; for( ::string const& phy : descr.phys ) abs_phys.push_back(mk_abs(phy,root_dir_s)) ;
		/**/                                    _create(report,view) ;
		for( ::string const& phy : descr.phys ) _create(report,phy ) ;
		if (is_dirname(view)) {
			for( ::string const& cu : descr.copy_up ) {
				::string dst = descr.phys[0]+cu ;
				if (is_dirname(cu))
					_create(report,dst) ;
				else
					for( size_t i=1 ; i<descr.phys.size() ; i++ )
						if (_create(report,dst,descr.phys[i]+cu)) break ;
			}
		}
		size_t          sz    = descr.phys.size() ;
		::string const& upper = descr.phys[0]     ;
		if (sz==1) {
			_mount_bind( abs_view , abs_phys[0] ) ;
		} else {
			::string work_s = is_lcl(upper) ? work_dir_s+"work_"+(work_idx++)+'/' : upper.substr(0,upper.size()-1)+".work/" ;  // if not in the repo, it must be in tmp
			mk_dir_s(work_s) ;
			_mount_overlay( abs_view , abs_phys , mk_abs(work_s,root_dir_s) ) ;
		}
	}
	trace("done") ;
	return true/*entered*/ ;
}

void JobSpace::exit() {
	Trace trace("JobSpace::exit") ;
	_fuse_store.clear() ;
	trace("done") ;
}

// XXX : implement recursive views
// for now, phys cannot englobe or lie within a view, but when it is to be implemented, it is here
::vmap_s<::vector_s> JobSpace::flat_phys() const {
	::vmap_s<::vector_s> res ; res.reserve(views.size()) ;
	for( auto const& [view,descr] : views ) res.emplace_back(view,descr.phys) ;
	return res ;
}

void JobSpace::mk_canon(::string const& phy_root_dir_s) {
	auto do_top = [&]( ::string& dir_s , bool slash_ok , ::string const& key )->void {
		if ( !dir_s                                     ) return ;
		if ( !is_canon(dir_s)                           ) dir_s = ::mk_canon(dir_s) ;
		if ( slash_ok && dir_s=="/"                     ) return ;
		if (             dir_s=="/"                     ) throw key+" cannot be /"                                           ;
		if ( !is_abs(dir_s)                             ) throw key+" must be absolute : "+no_slash(dir_s)                   ;
		if ( phy_root_dir_s.starts_with(dir_s         ) ) throw "repository cannot lie within "+key+' '+no_slash(dir_s)      ;
		if ( dir_s         .starts_with(phy_root_dir_s) ) throw key+' '+no_slash(dir_s)+" cannot be local to the repository" ;
	} ;
	//                   slash_ok
	do_top( chroot_dir_s , true  , "chroot dir" ) ;
	do_top( root_view_s  , false , "root view"  ) ;
	do_top( tmp_view_s   , false , "tmp view"   ) ;
	if ( +root_view_s && +tmp_view_s ) {
		if (root_view_s.starts_with(tmp_view_s )) throw "root view "+no_slash(root_view_s)+" cannot lie within tmp view " +no_slash(tmp_view_s ) ;
		if (tmp_view_s .starts_with(root_view_s)) throw "tmp view " +no_slash(tmp_view_s )+" cannot lie within root view "+no_slash(root_view_s) ;
	}
	//
	::string const& job_root_dir_s = +root_view_s ? root_view_s : phy_root_dir_s ;
	auto do_path = [&](::string& path)->void {
		if      (!is_canon(path)                 ) path = ::mk_canon(path)                   ;
		if      (path.starts_with("../")         ) path = mk_abs(path,job_root_dir_s)        ;
		else if (path.starts_with(job_root_dir_s)) path = path.substr(job_root_dir_s.size()) ;
	} ;
	for( auto& [view,_] : views ) {
		do_path(view) ;
		if (!view                           ) throw "cannot map the whole repository"s                  ;
		if (job_root_dir_s.starts_with(view)) throw "repository cannot lie within view "+no_slash(view) ;
	}
	//
	for( auto& [view,descr] : views ) {
		bool is_dir_view = is_dirname(view)  ;
		/**/                             if ( !is_dir_view && descr.phys.size()!=1                                     ) throw "cannot map non-dir " +no_slash(view)+" to an overlay" ;
		for( auto const& [v,_] : views ) if ( &v!=&view && view.starts_with(v) && (v.back()=='/'||view[v.size()]=='/') ) throw "cannot map "+no_slash(view)+" within "+v              ;
		bool lcl_view = _is_lcl_tmp(view) ;
		for( ::string& phy : descr.phys ) {
			do_path(phy) ;
			if ( !lcl_view && _is_lcl_tmp(phy)    ) throw "cannot map external view "+no_slash(view)+" to local or tmp "+no_slash(phy) ;
			if (  is_dir_view && !is_dirname(phy) ) throw "cannot map dir "          +no_slash(view)+" to file "        +no_slash(phy) ;
			if ( !is_dir_view &&  is_dirname(phy) ) throw "cannot map file "         +no_slash(view)+" to dir "         +no_slash(phy) ;
			if (+phy) {
				for( auto const& [v,_] : views ) {                                                                            // XXX : suppress this check when recursive maps are implemented
					if ( phy.starts_with(v  ) && (v  .back()=='/'||phy[v  .size()]=='/') ) throw "cannot map "+no_slash(view)+" to "+no_slash(phy)+" within "    +no_slash(v) ;
					if ( v  .starts_with(phy) && (phy.back()=='/'||v  [phy.size()]=='/') ) throw "cannot map "+no_slash(view)+" to "+no_slash(phy)+" containing "+no_slash(v) ;
				}
			} else {
				for( auto const& [v,_] : views )                                                                              // XXX : suppress this check when recursive maps are implemented
					if (!is_abs(v)) throw "cannot map "+no_slash(view)+" to full repository with "+no_slash(v)+" being map" ;
			}
		}
	}
}

//
// JobRpcReq
//

::ostream& operator<<( ::ostream& os , TargetDigest const& td ) {
	const char* sep = "" ;
	/**/                    os << "TargetDigest("      ;
	if ( td.pre_exist   ) { os <<      "pre_exist"     ; sep = "," ; }
	if (+td.tflags      ) { os <<sep<< td.tflags       ; sep = "," ; }
	if (+td.extra_tflags) { os <<sep<< td.extra_tflags ; sep = "," ; }
	if (+td.crc         ) { os <<sep<< td.crc          ; sep = "," ; }
	if (+td.sig         )   os <<sep<< td.sig          ;
	return                  os <<')'                   ;
}

::ostream& operator<<( ::ostream& os , JobDigest const& jd ) {
	return os << "JobDigest(" << jd.wstatus<<':'<<jd.status <<','<< jd.targets <<','<< jd.deps << ')' ;
}

::ostream& operator<<( ::ostream& os , JobRpcReq const& jrr ) {
	/**/                      os << "JobRpcReq(" << jrr.proc <<','<< jrr.seq_id <<','<< jrr.job ;
	switch (jrr.proc) {
		case JobRpcProc::Start : os <<','<< jrr.port                                                     ; break ;
		case JobRpcProc::End   : os <<','<< jrr.digest <<','<< jrr.phy_tmp_dir_s <<','<< jrr.dynamic_env ; break ;
		default                :                                                                           break ;
	}
	return                    os <<','<< jrr.msg <<')' ;
}

//
// JobRpcReply
//

::ostream& operator<<( ::ostream& os , MatchFlags const& mf ) {
	/**/             os << "MatchFlags(" ;
	switch (mf.is_target) {
		case Yes   : os << "target" ; if (+mf.tflags()) os<<','<<mf.tflags() ; if (+mf.extra_tflags()) os<<','<<mf.extra_tflags() ; break ;
		case No    : os << "dep,"   ; if (+mf.dflags()) os<<','<<mf.dflags() ; if (+mf.extra_dflags()) os<<','<<mf.extra_dflags() ; break ;
		case Maybe :                                                                                                                break ;
	DF}
	return           os << ')' ;
}

::ostream& operator<<( ::ostream& os , JobRpcReply const& jrr ) {
	os << "JobRpcReply(" << jrr.proc ;
	switch (jrr.proc) {
		case JobRpcProc::Start :
			/**/                           os <<','  << hex<<jrr.addr<<dec                ;
			/**/                           os <<','  << jrr.autodep_env                   ;
			if      (+jrr.job_space      ) os <<','  << jrr.job_space                     ;
			if      ( jrr.keep_tmp       ) os <<','  << "keep"                            ;
			if      ( jrr.tmp_sz_mb==Npos) os <<",T:"<< "..."                             ;
			else                           os <<",T:"<< jrr.tmp_sz_mb                     ;
			if      (+jrr.cwd_s          ) os <<','  << jrr.cwd_s                         ;
			if      (+jrr.date_prec      ) os <<','  << jrr.date_prec                     ;
			/**/                           os <<','  << mk_printable(fmt_string(jrr.env)) ; // env may contain the non-printable EnvPassMrkr value
			/**/                           os <<','  << jrr.interpreter                   ;
			/**/                           os <<','  << jrr.kill_sigs                     ;
			if      (jrr.live_out        ) os <<','  << "live_out"                        ;
			/**/                           os <<','  << jrr.method                        ;
			if      (+jrr.network_delay  ) os <<','  << jrr.network_delay                 ;
			if      (+jrr.pre_actions    ) os <<','  << jrr.pre_actions                   ;
			/**/                           os <<','  << jrr.small_id                      ;
			if      (+jrr.star_matches   ) os <<','  << jrr.star_matches                  ;
			if      (+jrr.deps           ) os <<'<'  << jrr.deps                          ;
			if      (+jrr.static_matches ) os <<'>'  << jrr.static_matches                ;
			if      (+jrr.stdin          ) os <<'<'  << jrr.stdin                         ;
			if      (+jrr.stdout         ) os <<'>'  << jrr.stdout                        ;
			if      (+jrr.timeout        ) os <<','  << jrr.timeout                       ;
			/**/                           os <<','  << jrr.cmd                           ; // last as it is most probably multi-line
			;
		break ;
	DN}
	return os << ')' ;
}

bool/*entered*/ JobRpcReply::enter(
		::vmap_s<MountAction>& actions                                                                                  // out
	,	::map_ss             & cmd_env                                                                                  // .
	,	::string             & phy_tmp_dir_s                                                                            // .
	,	::vmap_ss            & dynamic_env                                                                              // .
	,	pid_t                & first_pid                                                                                // .
	,	JobIdx                 job                                                                                      // in
	,	::string        const& phy_root_dir_s                                                                           // .
	,	SeqId                  seq_id                                                                                   // .
) {
	Trace trace("JobRpcReply::enter",job,phy_root_dir_s,seq_id) ;
	//
	for( auto& [k,v] : env ) {
		if      (v!=EnvPassMrkr)                                                             cmd_env[k] = ::move(v) ;
		else if (has_env(k)    ) { ::string v = get_env(k) ; dynamic_env.emplace_back(k,v) ; cmd_env[k] = ::move(v) ; } // if special illegal value, use value from environment (typically from slurm)
	}
	//
	if ( auto it=cmd_env.find("TMPDIR") ; it!=cmd_env.end()   ) {
		if (!is_abs(it->second)) throw "$TMPDIR must be absolute but is "+it->second ;
		phy_tmp_dir_s = with_slash(it->second)+key+'/'+small_id+'/' ;
	} else if (tmp_sz_mb==Npos) {
		phy_tmp_dir_s = phy_root_dir_s+PrivateAdminDirS+"tmp/"+small_id+'/' ;
	} else {
		phy_tmp_dir_s = {} ;
	}
	if      ( !phy_tmp_dir_s && tmp_sz_mb && !job_space.tmp_view_s ) throw "cannot create tmpfs of size "s+to_string_with_units<'M'>(tmp_sz_mb)+"B without tmp_view" ;
	if      ( keep_tmp                                             ) phy_tmp_dir_s = phy_root_dir_s+AdminDirS+"tmp/"+job+'/' ;
	else if ( +phy_tmp_dir_s                                       ) _tmp_dir_s_to_cleanup = phy_tmp_dir_s ;
	autodep_env.root_dir_s = +job_space.root_view_s ? job_space.root_view_s : phy_root_dir_s ;
	autodep_env.tmp_dir_s  = +job_space.tmp_view_s  ? job_space.tmp_view_s  : phy_tmp_dir_s  ;
	//
	try {
		if (+phy_tmp_dir_s) unlnk_inside_s(phy_tmp_dir_s,true/*abs_ok*/) ;             // ensure tmp dir is clean
	} catch (::string const&) {
		try                       { mk_dir_s(phy_tmp_dir_s) ;            }             // ensure tmp dir exists
		catch (::string const& e) { throw "cannot create tmp dir : "+e ; }
	}
	//
	cmd_env["PWD"        ] = no_slash(autodep_env.root_dir_s+cwd_s) ;
	cmd_env["ROOT_DIR"   ] = no_slash(autodep_env.root_dir_s      ) ;
	cmd_env["SEQUENCE_ID"] = ::to_string(seq_id  )                  ;
	cmd_env["SMALL_ID"   ] = ::to_string(small_id)                  ;
	if (PY_LD_LIBRARY_PATH[0]!=0) {
		auto [it,inserted] = cmd_env.try_emplace("LD_LIBRARY_PATH",PY_LD_LIBRARY_PATH) ;
		if (!inserted) it->second <<':'<< PY_LD_LIBRARY_PATH ;
	}
	//
	if (+autodep_env.tmp_dir_s) {
		cmd_env["TMPDIR"] = no_slash(autodep_env.tmp_dir_s) ;
	} else {
		SWEAR(!cmd_env.contains("TMPDIR")) ;                                           // if we have a TMPDIR env var, we should have a tmp dir
		autodep_env.tmp_dir_s = with_slash(P_tmpdir) ;                                 // detect accesses to P_tmpdir (usually /tmp) and generate an error
	}
	if (!cmd_env.contains("HOME")) cmd_env["HOME"] = no_slash(autodep_env.tmp_dir_s) ; // by default, set HOME to tmp dir as this cannot be set from rule
	//
	::string phy_work_dir_s = PrivateAdminDirS+"work/"s+small_id+'/'                                                                                                          ;
	bool     entered        = job_space.enter( actions , phy_root_dir_s , phy_tmp_dir_s , tmp_sz_mb , phy_work_dir_s , autodep_env.src_dirs_s , method==AutodepMethod::Fuse ) ;
	if (entered) {
		// find a good starting pid
		// the goal is to minimize risks of pid conflicts between jobs in case pid is used to generate unique file names as temporary file instead of using TMPDIR, which is quite common
		// to do that we spread pid's among the availale range by setting the first pid used by jos as apart from each other as possible
		// call phi the golden number and NPids the number of available pids
		// spreading is maximized by using phi*NPids as an elementary spacing and id (small_id) as an index modulo NPids
		// this way there is a conflict between job 1 and job 2 when (id2-id1)*phi is near an integer
		// because phi is the irrational which is as far from rationals as possible, and id's are as small as possible, this probability is minimized
		// note that this is over-quality : any more or less random number would do the job : motivation is mathematical beauty rather than practical efficiency
		static constexpr uint32_t FirstPid = 300                                 ;     // apparently, pid's wrap around back to 300
		static constexpr uint64_t NPids    = MAX_PID - FirstPid                  ;     // number of available pid's
		static constexpr uint64_t DelatPid = (1640531527*NPids) >> n_bits(NPids) ;     // use golden number to ensure best spacing (see above), 1640531527 = (2-(1+sqrt(5))/2)<<32
		first_pid = FirstPid + ((small_id*DelatPid)>>(32-n_bits(NPids)))%NPids ;       // DelatPid on 64 bits to avoid rare overflow in multiplication
	}
	return entered ;
}

void JobRpcReply::exit() {
	// work dir cannot be cleaned up as we may have chroot'ed inside
	Trace trace("JobRpcReply::exit",_tmp_dir_s_to_cleanup) ;
	if (+_tmp_dir_s_to_cleanup) unlnk_inside_s(_tmp_dir_s_to_cleanup,true/*abs_ok*/ ) ;
	job_space.exit() ;
}

//
// JobMngtRpcReq
//

::ostream& operator<<( ::ostream& os , JobMngtRpcReq const& jmrr ) {
	/**/                               os << "JobMngtRpcReq(" << jmrr.proc <<','<< jmrr.seq_id <<','<< jmrr.job <<','<< jmrr.fd ;
	switch (jmrr.proc) {
		case JobMngtProc::LiveOut    : os <<','<< jmrr.txt.size() ;                             break ;
		case JobMngtProc::ChkDeps    :
		case JobMngtProc::DepVerbose : os <<','<< jmrr.deps       ;                             break ;
		case JobMngtProc::Encode     : os <<','<< jmrr.min_len    ;                             [[fallthrough]] ;
		case JobMngtProc::Decode     : os <<','<< jmrr.ctx <<','<< jmrr.file <<','<< jmrr.txt ; break ;
		default                      :                                                          break ;
	}
	return                             os <<')' ;
}

//
// JobMngtRpcReply
//

::ostream& operator<<( ::ostream& os , JobMngtRpcReply const& jmrr ) {
	/**/                               os << "JobMngtRpcReply(" << jmrr.proc ;
	switch (jmrr.proc) {
		case JobMngtProc::ChkDeps    : os <<','<< jmrr.fd <<','<<                                   jmrr.ok ; break ;
		case JobMngtProc::DepVerbose : os <<','<< jmrr.fd <<','<< jmrr.dep_infos                            ; break ;
		case JobMngtProc::Decode     : os <<','<< jmrr.fd <<','<< jmrr.txt <<','<< jmrr.crc <<','<< jmrr.ok ; break ;
		case JobMngtProc::Encode     : os <<','<< jmrr.fd <<','<< jmrr.txt <<','<< jmrr.crc <<','<< jmrr.ok ; break ;
	DN}
	return                             os << ')' ;
}

//
// SubmitAttrs
//

::ostream& operator<<( ::ostream& os , SubmitAttrs const& sa ) {
	const char* sep = "" ;
	/**/                 os << "SubmitAttrs("          ;
	if (+sa.tag      ) { os <<      sa.tag       <<',' ; sep = "," ; }
	if ( sa.live_out ) { os <<sep<< "live_out,"        ; sep = "," ; }
	if ( sa.n_retries) { os <<sep<< sa.n_retries <<',' ; sep = "," ; }
	if (+sa.pressure ) { os <<sep<< sa.pressure  <<',' ; sep = "," ; }
	if (+sa.deps     ) { os <<sep<< sa.deps      <<',' ; sep = "," ; }
	if (+sa.reason   )   os <<sep<< sa.reason    <<',' ;
	return               os <<')'                      ;
}

//
// JobInfo
//

::ostream& operator<<( ::ostream& os , JobInfoStart const& jis ) {
	return os << "JobInfoStart(" << jis.submit_attrs <<','<< jis.rsrcs <<','<< jis.pre_start <<','<< jis.start <<')' ;
}

::ostream& operator<<( ::ostream& os , JobInfoEnd const& jie ) {
	return os << "JobInfoEnd(" << jie.end <<')' ;
}

JobInfo::JobInfo(::string const& filename) {
	try {
		IFStream job_stream { filename } ;
		deserialize(job_stream,start) ;
		deserialize(job_stream,end  ) ;
	} catch (...) {}                    // we get what we get
}

void JobInfo::write(::string const& filename) const {
	OFStream os{dir_guard(filename)} ;
	serialize(os,start) ;
	serialize(os,end  ) ;
}
//
// codec
//


namespace Codec {

	::string mk_decode_node( ::string const& file , ::string const& ctx , ::string const& code ) {
		return CodecPfx+mk_printable<'.'>(file)+".cdir/"+mk_printable<'.'>(ctx)+".ddir/"+mk_printable(code) ;
	}

	::string mk_encode_node( ::string const& file , ::string const& ctx , ::string const& val ) {
		return CodecPfx+mk_printable<'.'>(file)+".cdir/"+mk_printable<'.'>(ctx)+".edir/"+::string(Xxh(val).digest()) ;
	}

	::string mk_file(::string const& node) {
		return parse_printable<'.'>(node,::ref(size_t(0))).substr(sizeof(CodecPfx)-1) ; // account for terminating null in CodecPfx
	}

}
