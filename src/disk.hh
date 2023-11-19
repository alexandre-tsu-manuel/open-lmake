// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#pragma once

#include "fd.hh"
#include "lib.hh"
#include "time.hh"

namespace Disk {
	using Ddate  = Time::Ddate ;
	using DiskSz = uint64_t    ;

	ENUM(Access
	,	Lnk        // file is accessed with readlink
	,	Reg        // file is accessed with open
	,	Stat       // file is accessed with stat like (read inode)
	)
	static constexpr char AccessChars[] = {
		'L'                                                // Lnk
	,	'R'                                                // Reg
	,	'T'                                                // Stat
	} ;
	static_assert(::size(AccessChars)==+Access::N) ;
	using Accesses = BitMap<Access> ;
	static constexpr Accesses DataAccesses { Access::Lnk , Access::Reg } ;

	ENUM_1( FileTag
	,	None
	,	Reg
	,	Exe        // a regular file with exec permission
	,	Lnk
	,	Dir
	,	Err
	)

	struct FileInfoDate ;
	struct FileInfo {
		friend ::ostream& operator<<( ::ostream& , FileInfo const& ) ;
		friend FileInfoDate ;
		using Stat = struct ::stat ;
	private :
		// statics
		static Stat _s_stat( Fd at , const char* name ) {
			Stat st ;
			errno = 0 ;
			::fstatat( at , name , &st , AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW ) ;
			return st ;
		}
		// cxtors & casts
	public :
		FileInfo(                              ) = default ;
		FileInfo( Fd at                        ) : FileInfo{        at     ,{}           } {}
		FileInfo(         ::string const& name ) : FileInfo{        Fd::Cwd,name         } {}
		FileInfo( Fd at , ::string const& name ) : FileInfo{_s_stat(at     ,name.c_str())} {}
	private :
		FileInfo(Stat const&) ;                                       // /!\ errno must be valid and 0 if stat is ok
		// accesses
	public :
		bool operator+() const {
			switch (tag) {
				case FileTag::Reg :
				case FileTag::Exe :
				case FileTag::Lnk : return true  ;
				default           : return false ;
			}
		}
		bool operator!() const { return !+*this                                ; } // i.e. sz & date are not present
		bool is_reg   () const { return tag==FileTag::Reg || tag==FileTag::Exe ; }
		// data
		DiskSz  sz  = 0             ;
		FileTag tag = FileTag::None ;
	} ;

	struct FileInfoDate : FileInfo {
		friend ::ostream& operator<<( ::ostream& , FileInfoDate const& ) ;
		// cxtors & casts
		FileInfoDate(                              ) = default ;
		FileInfoDate( Fd at                        ) : FileInfoDate(at     ,{}  ) {} // opening file is important to guarantee attribute sync with NFS
		FileInfoDate(         ::string const& name ) : FileInfoDate(Fd::Cwd,name) {} // .
		FileInfoDate( Fd at , ::string const& name ) ;                               // .
		// services
		Ddate date_or_now() const { return +*this ? date : Time::Ddate::s_now() ; }
		// data
		Ddate date ;
	} ;

	::string dir_name (::string const&) ;
	::string base_name(::string const&) ;

	::vector_s read_lines   ( ::string const& file                     ) ;
	::string   read_content ( ::string const& file                     ) ;
	void       write_lines  ( ::string const& file , ::vector_s const& ) ;
	void       write_content( ::string const& file , ::string   const& ) ;

	// list files within dir with prefix in front of each entry
	::vector_s lst_dir( Fd at , ::string const& dir={} , ::string const& prefix={} ) ;
	// deep list files within dir with prefix in front of each entry, return a single entry {prefix} if file is not a dir (including if file does not exist)
	::vector_s walk( Fd at , ::string const& file , ::string const& prefix={} ) ;
	//
	void            make_dir     ( Fd at , ::string const& dir  , bool unlink_ok=false ) ; // if unlink <=> unlink any file on the path if necessary to make dir
	::string const& dir_guard    ( Fd at , ::string const& file                        ) ; // return file
	void            unlink_inside( Fd at , ::string const& dir ={}                     ) ;
	void            unlink       ( Fd at , ::string const& file                        ) ; // unlink whole dir if it is one
	//
	static inline void lnk( Fd at , ::string const& file , ::string const& target ) {
		if (::symlinkat(target.c_str(),at,file.c_str())!=0) {
			::string at_str = at==Fd::Cwd ? ""s : to_string('<',int(at),">/") ;
			throw to_string("cannot create symlink from ",at_str,file," to ",target) ;
		}
	}

	static inline Fd open_read( Fd at , ::string const& filename ) {
		return ::openat( at , filename.c_str() , O_RDONLY|O_CLOEXEC , 0666 ) ;
	}

	static inline Fd open_write( Fd at , ::string const& filename , bool append=false , bool exe=false , bool read_only=false ) {
		dir_guard(at,filename) ;
		return ::openat( at , filename.c_str() , O_WRONLY|O_CREAT|O_CLOEXEC|(append?O_APPEND:O_TRUNC) , 0777 & ~(exe?0000:0111) & ~(read_only?0222:0000) ) ;
	}

	static inline ::string read_lnk( Fd at , ::string const& file ) {
		char    buf[PATH_MAX] ;
		ssize_t cnt           = ::readlinkat(at,file.c_str(),buf,PATH_MAX) ;
		if ( cnt<0 || cnt>=PATH_MAX ) return {} ;
		return {buf,size_t(cnt)} ;
	}

	static inline bool  is_reg   ( Fd at , ::string const& file={} ) { return  FileInfo    (at,file).is_reg()          ; }
	static inline bool  is_lnk   ( Fd at , ::string const& file={} ) { return  FileInfo    (at,file).tag==FileTag::Lnk ; }
	static inline bool  is_dir   ( Fd at , ::string const& file={} ) { return  FileInfo    (at,file).tag==FileTag::Dir ; }
	static inline bool  is_target( Fd at , ::string const& file={} ) { return +FileInfo    (at,file)                   ; }
	static inline bool  is_exe   ( Fd at , ::string const& file={} ) { return  FileInfo    (at,file).tag==FileTag::Exe ; }
	static inline bool  is_none  ( Fd at , ::string const& file={} ) { return !FileInfo    (at,file).tag               ; }
	static inline Ddate file_date( Fd at , ::string const& file={} ) { return  FileInfoDate(at,file).date              ; }

	static inline ::vector_s      lst_dir      ( ::string const& dir  , ::string const& prefix={}          ) { return lst_dir      (Fd::Cwd,dir ,prefix    ) ; }
	static inline ::vector_s      walk         ( ::string const& file , ::string const& prefix={}          ) { return walk         (Fd::Cwd,file,prefix    ) ; }
	static inline void            make_dir     ( ::string const& dir  , bool unlink_ok=false               ) { return make_dir     (Fd::Cwd,dir ,unlink_ok ) ; }
	static inline ::string const& dir_guard    ( ::string const& file                                      ) { return dir_guard    (Fd::Cwd,file           ) ; } // return file
	static inline void            unlink_inside( ::string const& dir                                       ) {        unlink_inside(Fd::Cwd,dir            ) ; }
	static inline void            unlink       ( ::string const& file                                      ) {        unlink       (Fd::Cwd,file           ) ; }
	static inline void            lnk          ( ::string const& file , ::string const& target             ) {        lnk          (Fd::Cwd,file,target    ) ; }
	static inline Fd              open_read    ( ::string const& file                                      ) { return open_read    (Fd::Cwd,file           ) ; }
	static inline Fd              open_write   ( ::string const& file , bool append=false , bool exe=false ) { return open_write   (Fd::Cwd,file,append,exe) ; }
	static inline ::string        read_lnk     ( ::string const& file                                      ) { return read_lnk     (Fd::Cwd,file           ) ; }
	static inline bool            is_reg       ( ::string const& file                                      ) { return is_reg       (Fd::Cwd,file           ) ; }
	static inline bool            is_lnk       ( ::string const& file                                      ) { return is_lnk       (Fd::Cwd,file           ) ; }
	static inline bool            is_dir       ( ::string const& file                                      ) { return is_dir       (Fd::Cwd,file           ) ; }
	static inline bool            is_target    ( ::string const& file                                      ) { return is_target    (Fd::Cwd,file           ) ; }
	static inline bool            is_exe       ( ::string const& file                                      ) { return is_exe       (Fd::Cwd,file           ) ; }
	static inline bool            is_none      ( ::string const& file                                      ) { return is_none      (Fd::Cwd,file           ) ; }
	static inline Ddate           file_date    ( ::string const& file                                      ) { return file_date    (Fd::Cwd,file           ) ; }

	static inline ::string cwd() {
		char* buf = ::getcwd(nullptr,0) ;
		if (!buf) throw "cannot get cwd"s ;
		::string res{buf} ;
		::free(buf) ;
		SWEAR( res[0]=='/' , res[0] ) ;
		if (res.size()==1) return {}  ;                                        // cwd_ contains components prefixed by /, if at root, it is logical for it to be empty
		else               return res ;
	}

	static inline bool is_abs  (::string const& name  ) { return name.empty() || name  [0]=='/'   ; } // name   is <x>(/<x>)* or (/<x>)*  with <x>=[^/]+, empty name   is necessarily absolute
	static inline bool is_abs_s(::string const& name_s) { return                 name_s[0]=='/'   ; } // name_s is (<x>/)*    or /(<x>/)* with <x>=[^/]+, empty name_s is necessarily relative
	//
	static inline bool is_lcl  (::string const& name  ) { return !( is_abs  (name  ) || name  .starts_with("../") || name==".." ) ; }
	static inline bool is_lcl_s(::string const& name_s) { return !( is_abs_s(name_s) || name_s.starts_with("../")               ) ; }
	//
	/**/          ::string mk_lcl( ::string const& file , ::string const& dir_s ) ; // return file (passed as from dir_s origin) as seen from dir_s
	/**/          ::string mk_glb( ::string const& file , ::string const& dir_s ) ; // return file (passed as from dir_s       ) as seen from dir_s origin
	static inline ::string mk_abs( ::string const& file , ::string const& dir_s ) { // return file (passed as from dir_s       ) as absolute
		SWEAR( is_abs_s(dir_s) , dir_s ) ;
		return mk_glb(file,dir_s) ;
	}
	static inline ::string mk_rel( ::string const& file , ::string const& dir_s ) {
		if (is_abs(file)==is_abs_s(dir_s)) return mk_lcl(file,dir_s) ;
		else                               return file               ;
	}

	struct FileMap {
		FileMap( Fd , ::string const&   ) ;
		FileMap(      ::string const& f ) : FileMap{Fd::Cwd,f} {}
		operator bool() const { return _ok ; }
		// data
		const uint8_t* data = nullptr ;
		size_t         sz   = 0       ;
	private :
		AutoCloseFd _fd ;
		bool        _ok = false ;
	} ;

	ENUM_1(Kind
	,	Dep = SrcDirs                  // <=Dep means that file must be reported as a dep
	,	Repo
	,	SrcDirs                        // file has been found in source dirs
	,	Root                           // file is the root dir
	,	Tmp
	,	Proc                           // file is in /proc
	,	Admin
	,	Ext                            // all other cases
	)

	struct RealPathEnv {
		friend ::ostream& operator<<( ::ostream& , RealPathEnv const& ) ;
		LnkSupport lnk_support = LnkSupport::Full ;                            // by default, be pessimistic
		::string   root_dir    = {}               ;
		::string   tmp_dir     = {}               ;
		::string   tmp_view    = {}               ;
		::vector_s src_dirs_s  = {}               ;
	} ;

	// XXX : avoid duplicating RealPathEnv by storing a pointer to it here rather than inheritance, which requires to cleanly separate global part (in RealPathEnv) and specific part (in RealPath)
	struct RealPath : RealPathEnv {
		struct SolveReport {
			friend ::ostream& operator<<( ::ostream& , SolveReport const& ) ;
			// data
			::string   real   = {}        ;                // real path relative to root if in_repo or found in a relative src_dir ...
			//                                             // ... or absolute if found in an absolute src_dir or mapped in tmp, else empty
			::vector_s lnks   = {}        ;                // links followed to get to real
			Kind       kind   = Kind::Ext ;                // do not process awkard files
			bool       mapped = false     ;                // if true <=> tmp mapping has been used
		} ;
	private :
		// helper class to help recognize when we are in repo or in tmp
		struct _Dvg {
			_Dvg( ::string const& domain , ::string const& chk ) { update(domain,chk) ; }
			operator bool() const { return ok ; }
			// udpate after domain & chk have been lengthened or shortened, but not modified internally
			void update( ::string const& domain , ::string const& chk ) {
				size_t start = dvg ;;
				ok  = domain.size() <= chk.size()     ;
				dvg = ok ? domain.size() : chk.size() ;
				for( size_t i=start ; i<dvg ; i++ ) {
					if (domain[i]!=chk[i]) {
						ok  = false ;
						dvg = i     ;
						return ;
					}
				}
				if ( domain.size() < chk.size() ) ok = chk[domain.size()]=='/' ;
			}
			bool   ok  = false ;
			size_t dvg = 0     ;
		} ;

		// statics
	private :
		// if No <=> no file, if Maybe <=> a regular file, if Yes <=> a link
		Bool3/*ok*/ _read_lnk( ::string& target/*out*/ , ::string const& real ) {
			::string t = read_lnk(real) ;
			if (t.empty()) return Maybe & (errno!=ENOENT) ;
			target = ::move(t) ;
			return Yes ;
		}
		// cxtors & casts
	public :
		RealPath() = default ;
		// src_dirs_s may be either absolute or relative, but must be canonic
		// tmp_dir and tmp_view must be absolute and canonic
		RealPath ( RealPathEnv const& rpe , pid_t p=0 ) { init(rpe,p) ; }
		void init( RealPathEnv const&     , pid_t  =0 ) ;
		// services
		SolveReport solve( Fd at , ::string const&      , bool no_follow=false ) ;
		SolveReport solve( Fd at , const char*     file , bool no_follow=false ) { return solve(at     ,::string(file),no_follow) ; } // ensure proper types
		SolveReport solve(         ::string const& file , bool no_follow=false ) { return solve(Fd::Cwd,         file ,no_follow) ; }
		SolveReport solve( Fd at ,                        bool no_follow=false ) { return solve(at     ,         {}   ,no_follow) ; }
		//
		vmap_s<Accesses> exec(SolveReport&) ;                                  // arg is updated to reflect last interpreter
	private :
		::string _find_src(::string const& real) const ;
		// data
	public :
		pid_t    pid          = 0                ;
		bool     has_tmp_view = false/*garbage*/ ;
		::string cwd_         ;
	private :
		::string   _admin_dir      ;
		::vector_s _abs_src_dirs_s ;                                          // this is an absolute version of src_dirs
	} ;
	::ostream& operator<<( ::ostream& , RealPath::SolveReport const& ) ;

}
