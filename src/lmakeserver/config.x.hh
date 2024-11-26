// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "serialize.hh"

#include "rpc_client.hh"

#ifdef STRUCT_DECL

ENUM( CacheTag // PER_CACHE : add a tag for each cache method
,	None
,	Dir
)

ENUM( Color
,	None
,	HiddenNote
,	HiddenOk
,	Note
,	Ok
,	Warning
,	SpeculateErr
,	Err
)

ENUM( ConfigDiff
,	None         // configs are identical
,	Dynamic      // config can be updated while engine runs
,	Static       // config can be updated when engine is steady
,	Clean        // config cannot be updated (requires clean repo)
)

ENUM( StdRsrc
,	Cpu
,	Mem
,	Tmp
)

namespace Engine {

	struct Config ;

}

#endif
#ifdef STRUCT_DEF

namespace Engine {

	struct Version {
		static const Version Db ;
		bool operator==(Version const&) const = default ;
		size_t major = 0 ;
		size_t minor = 0 ;
	} ;
	constexpr Version Version::Db = {1,0} ;

	// changing these values require restarting from a clean base
	struct ConfigClean {
		// services
		bool operator==(ConfigClean const&) const = default ;
		// data
		// START_OF_VERSIONING
		Version    db_version             ;                    // must always stay first so it is always understood, by default, db version does not match
		LnkSupport lnk_support            = LnkSupport::Full ;
		::string   user_local_admin_dir_s ;
		::string   key                    ;                    // random key to differentiate repo from other repos
		// END_OF_VERSIONING
	} ;

	// changing these can only be done when lmake is not running
	struct ConfigStatic {
		//
		struct Cache {
			friend ::string& operator+=( ::string& , Backend const& ) ;
			using Tag = CacheTag ;
			// cxtors & casts
			Cache() = default ;
			Cache( Py::Dict const& py_map ) ;
			// services
			bool operator==(Cache const&) const = default ;
			template<IsStream T> void serdes(T& s) {
				::serdes(s,tag) ;
				::serdes(s,dct) ;
			}
			// data
			// START_OF_VERSIONING
			Caches::Tag tag ;
			::vmap_ss   dct ;
			// END_OF_VERSIONING
		} ;
		//
		struct TraceConfig {
			bool operator==(TraceConfig const&) const = default ;
			// START_OF_VERSIONING
			size_t   sz       = 100<<20      ;
			Channels channels = DfltChannels ;
			JobIdx   n_jobs   = 1000         ;
			// END_OF_VERSIONING
		} ;
		//
		// services
		bool operator==(ConfigStatic const&) const = default ;
		// data
		// START_OF_VERSIONING
		Time::Delay    date_prec       ;                                              // precision of dates on disk
		Time::Delay    heartbeat       ;                                              // min time between successive heartbeat probes for any given job
		Time::Delay    heartbeat_tick  ;                                              // min time between successive heartbeat probes
		DepDepth       max_dep_depth   = 1000 ; static_assert(DepDepth(1000)==1000) ; // ensure default value can be represented
		Time::Delay    network_delay   ;
		size_t         path_max        = -1    ;                                      // if -1 <=> unlimited
		::vector_s     sub_repos_s     ;
		TraceConfig    trace           ;
		::map_s<Cache> caches          ;
		bool           has_split_rules = false ;                                      // if true <=> read independently of config
		bool           has_split_srcs  = false ;                                      // .
		// END_OF_VERSIONING
	} ;

	// changing these can be made dynamically (i.e. while lmake is running)
	struct ConfigDynamic {
		//
		struct Backend {
			friend ::string& operator+=( ::string& , Backend const& ) ;
			using Tag = BackendTag ;
			// cxtors & casts
			Backend() = default ;
			Backend(Py::Dict const& py_map) ;
			// services
			bool operator==(Backend const&) const = default ;
			template<IsStream T> void serdes(T& s) {
				::serdes(s,ifce      ) ;
				::serdes(s,dct       ) ;
				::serdes(s,configured) ;
			}
			// data
			// START_OF_VERSIONING
			::string  ifce       ;
			::vmap_ss dct        ;
			bool      configured = false ;
			// END_OF_VERSIONING
		} ;
		//
		struct Console {
			bool operator==(Console const&) const = default ;
			uint8_t date_prec     = -1    ;                   // -1 means no date at all in console output
			uint8_t host_len      = 0     ;                   //  0 means no host at all in console output
			bool    has_exec_time = false ;
			bool    show_eta      = false ;
		} ;
		//
		// services
		bool operator==(ConfigDynamic const&) const = default ;
		bool   errs_overflow(size_t n) const { return n>max_err_lines ;                                       }
		size_t n_errs       (size_t n) const { if (errs_overflow(n)) return max_err_lines-1 ; else return n ; }
		// data
		// START_OF_VERSIONING
		size_t                                                                  max_err_lines = 0     ; // unlimited
		size_t                                                                  n_output_days = 0     ; // unlimited
		bool                                                                    reliable_dirs = false ; // if true => dirs coherence is enforced when files are modified
		Console                                                                 console       ;
		::array<uint8_t,N<StdRsrc>>                                             rsrc_digits   = {}    ; // precision of standard resources
		::array<Backend,N<BackendTag>>                                          backends      ;         // backend may refuse dynamic modification
		::array<::array<::array<uint8_t,3/*RGB*/>,2/*reverse_video*/>,N<Color>> colors        = {}    ;
		::umap_ss                                                               dbg_tab       = {}    ; // maps debug keys to modules to import
		// END_OF_VERSIONING
	} ;

	struct Config : ConfigClean , ConfigStatic , ConfigDynamic {
		friend ::string& operator+=( ::string& , Config const& ) ;
		// cxtors & casts
		Config(                      ) : booted{false} {}   // if config comes from nowhere, it is not booted
		Config(Py::Dict const& py_map) ;
		// services
		template<IsStream S> void serdes(S& s) {
			// START_OF_VERSIONING
			::serdes(s,static_cast<ConfigClean  &>(self)) ; // must always stay first field to ensure db_version is always understood
			::serdes(s,static_cast<ConfigStatic &>(self)) ;
			::serdes(s,static_cast<ConfigDynamic&>(self)) ;
			// END_OF_VERSIONING
			if (IsIStream<S>) booted = true ;               // is config comes from disk, it is booted
		}
		::string pretty_str() const ;
		void open(bool dynamic) ;
		ConfigDiff diff(Config const& other) {
			if (!(ConfigClean  ::operator==(other))) return ConfigDiff::Clean   ;
			if (!(ConfigStatic ::operator==(other))) return ConfigDiff::Static  ;
			if (!(ConfigDynamic::operator==(other))) return ConfigDiff::Dynamic ;
			else                                     return ConfigDiff::None    ;
		}
		// data (derived info not saved on disk)
		bool     booted            = false ;                // a marker to distinguish clean repository
		::string local_admin_dir_s ;
	} ;

}

#endif