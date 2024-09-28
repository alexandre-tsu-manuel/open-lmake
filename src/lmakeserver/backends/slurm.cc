// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include <dlfcn.h>

#include <filesystem>

#include <slurm/slurm.h>

#include "ext/cxxopts.hpp"

#include "generic.hh"

using namespace Disk ;

namespace Backends::Slurm {

	static constexpr int SlurmSpawnTrials  = 15 ;
	static constexpr int SlurmCancelTrials = 10 ;

	//
	// daemon info
	//

	struct Daemon {
		friend ::ostream& operator<<( ::ostream& , Daemon const& ) ;
		// data
		Pdate           time_origin { "2023-01-01 00:00:00" } ; // this leaves room til 2091
		float           nice_factor { 1                     } ; // conversion factor in the form of number of nice points per second
		::map_s<size_t> licenses    ;                           // licenses sampled from daemon
		bool            manage_mem  = false/*garbage*/        ;
	} ;

	//
	// resources
	//

	struct RsrcsDataSingle {
		friend ::ostream& operator<<( ::ostream& , RsrcsDataSingle const& ) ;
		// accesses
		bool operator==(RsrcsDataSingle const&) const = default ;
		// data
		uint16_t cpu      = 0  ; // number of logical cpu  (sbatch    --cpus-per-task option)
		uint32_t mem      = 0  ; // memory   in MB         (sbatch    --mem           option) default : illegal (memory reservation is compulsery)
		uint32_t tmp      = -1 ; // tmp disk in MB         (sbatch    --tmp           option) default : dont manage tmp size (provide infinite storage, reserv none)
		::string excludes ;      // list of excludes nodes (sbatch -x,--exclude       option)
		::string feature  ;      // feature/contraint      (sbatch -C,--constraint    option)
		::string gres     ;      // generic resources      (sbatch    --gres          option)
		::string licenses ;      // licenses               (sbtach -L,--licenses      option)
		::string nodes    ;      // list of required nodes (sbatch -w,--nodelist      option)
		::string part     ;      // partition name         (sbatch -p,--partition     option)
		::string qos      ;      // quality of service     (sbtach -q,--qos           option)
		::string reserv   ;      // reservation            (sbtach -r,--reservation   option)
	} ;

	struct RsrcsData : ::vector<RsrcsDataSingle> {
		using Base = ::vector<RsrcsDataSingle> ;
		// cxtors & casts
		RsrcsData(                               ) = default ;
		RsrcsData( ::vmap_ss&& , Daemon , JobIdx ) ;
		// services
		::vmap_ss mk_vmap(void) const ;
	} ;

	RsrcsData blend( RsrcsData&& rsrcs , RsrcsData const& force ) ;

}

namespace std {
	template<> struct hash<Backends::Slurm::RsrcsData> {
		size_t operator()(Backends::Slurm::RsrcsData const& rs) const {
			Hash::Xxh h{rs.size()} ;
			for( auto r : rs ) {
				h.update(r.cpu     ) ;
				h.update(r.mem     ) ;
				h.update(r.tmp     ) ;
				h.update(r.feature ) ;
				h.update(r.gres    ) ;
				h.update(r.licenses) ;
				h.update(r.part    ) ;
			}
			return +h.digest() ;
		}
	} ;
}

//
// SlurmBackend
//

namespace Backends::Slurm {

	using p_cxxopts = ::unique_ptr<cxxopts::Options> ;
	using SlurmId   = uint32_t                       ;

	Mutex<MutexLvl::Slurm> _slurm_mutex ; // ensure no more than a single outstanding request to daemon

	void slurm_init(const char* config_file) ;

	p_cxxopts                 create_parser     (                        ) ;
	RsrcsData                 parse_args        (::string const& args    ) ;
	void                      slurm_cancel      (SlurmId         slurm_id) ;
	::pair_s<Bool3/*job_ok*/> slurm_job_state   (SlurmId         slurm_id) ;
	::string                  read_stderr       (JobIdx                  ) ;
	Daemon                    slurm_sense_daemon(                        ) ;
	//
	SlurmId slurm_spawn_job( ::stop_token , ::string const& key , JobIdx , ::vector<ReqIdx> const& , int32_t nice , ::vector_s const& cmd_line , RsrcsData const& rsrcs , bool verbose ) ;

	p_cxxopts g_optParse = create_parser() ;

	constexpr Tag MyTag = Tag::Slurm ;

	struct SlurmBackend
	:	             GenericBackend<MyTag,SlurmId,RsrcsData,RsrcsData,false/*IsLocal*/>
	{	using Base = GenericBackend<MyTag,SlurmId,RsrcsData,RsrcsData,false/*IsLocal*/> ;

		struct SpawnedMap : ::umap<Rsrcs,JobIdx> {
			// count number of jobs spawned but not started yet
			// no entry is equivalent to entry with 0
			void inc(Rsrcs rs) {
				try_emplace(rs,0).first->second++ ; // create 0 entry if necessary
			}
			void dec(Rsrcs rs) {                    // entry must exist
				auto sit = find(rs) ;
				if(!--sit->second) erase(sit) ;     // no entry means 0, so collect when possible (questionable)
			}
			JobIdx n_spawned(Rsrcs rs) {
				auto it = find(rs) ;
				if (it==end()) return 0          ;  // no entry means 0
				else           return it->second ;
			}
		} ;

		// init
		static void s_init() {
			static bool once=false ; if (once) return ; else once = true ;
			s_register(MyTag,*new SlurmBackend) ;
		}

		// static data
		static DequeThread<SlurmId> _s_slurm_cancel_thread ; // when a req is killed, a lot of queued jobs may be canceled, better to do it in a separate thread

		// accesses

		virtual bool call_launch_after_start() const { return true ; }

		// services

		virtual void sub_config( vmap_ss const& dct , bool dynamic ) {
			Trace trace(BeChnl,"Slurm::config",STR(dynamic),dct) ;
			//
			const char* config_file = nullptr ;
			repo_key = base_name(no_slash(*g_root_dir_s))+':' ; // cannot put this code directly as init value as g_root_dir_s is not available early enough
			for( auto const& [k,v] : dct ) {
				try {
					switch (k[0]) {
						case 'c' : if(k=="config"           ) { config_file       = v.c_str()                ; continue ; } break ;
						case 'n' : if(k=="n_max_queued_jobs") { n_max_queued_jobs = from_string<uint32_t>(v) ; continue ; } break ;
						case 'r' : if(k=="repo_key"         ) { repo_key          =                       v  ; continue ; } break ;
						case 'u' : if(k=="use_nice"         ) { use_nice          = from_string<bool    >(v) ; continue ; } break ;
						default : ;
					}
				} catch (::string const& e) { trace("bad_val",k,v) ; throw "wrong value for entry "   +k+": "+v ; }
				/**/                        { trace("bad_key",k  ) ; throw "unexpected config entry: "+k        ; }
			}
			if (!dynamic) {
				slurm_init(config_file) ;
				daemon = slurm_sense_daemon() ;
				_s_slurm_cancel_thread.open('C',slurm_cancel) ;
			}
			trace("done") ;
		}

		virtual ::vmap_ss descr() const {
			::vmap_ss res {
				{ "manage memory" , daemon.manage_mem?"true":"false" }
			} ;
			for( auto const& [k,v] : daemon.licenses ) res.emplace_back(k,::to_string(v)) ;
			return res ;
		}

		virtual ::vmap_s<size_t> n_tokenss() const {
			return mk_vmap(daemon.licenses) ;
		}

		virtual ::vmap_ss mk_lcl( ::vmap_ss&& rsrcs , ::vmap_s<size_t> const& capacity ) const {
			bool             single = false             ;
			::umap_s<size_t> capa   = mk_umap(capacity) ;
			::umap_s<size_t> rs     ;
			for( auto&& [k,v] : rsrcs ) {
				if      ( capa.contains(k)                     ) { size_t s = from_string_rsrc<size_t>(k,v) ; rs[::move(k)] = s ; } // capacities of local backend are only integer information
				else if ( k=="gres" && !v.starts_with("shard") ) { single = true ;                                                }
			}
			::vmap_ss res ;
			if (single) for( auto&& [k,v] : rs ) { ::string s = to_string_rsrc(k,        capa[k] ) ; res.emplace_back( ::move(k) , ::move(s) ) ; }
			else        for( auto&& [k,v] : rs ) { ::string s = to_string_rsrc(k,::min(v,capa[k])) ; res.emplace_back( ::move(k) , ::move(s) ) ; }
			return res ;
		}

		virtual void open_req( ReqIdx req , JobIdx n_jobs ) {
			Base::open_req(req,n_jobs) ;
			grow(req_forces,req) = parse_args(Req(req)->options.flag_args[+ReqFlag::Backend]) ;
		}

		virtual void close_req(ReqIdx req) {
			Base::close_req(req) ;
			if(!reqs) SWEAR(!spawned_rsrcs,spawned_rsrcs) ;
		}

		virtual ::vmap_ss export_( RsrcsData const& rs                           ) const { return rs.mk_vmap()                                      ; }
		virtual RsrcsData import_( ::vmap_ss     && rsa , ReqIdx req , JobIdx ji ) const { return blend( {::move(rsa),daemon,ji} ,req_forces[req] ) ; }
		//
		virtual bool/*ok*/ fit_now(RsrcsAsk const& rsa) const {
			bool res = spawned_rsrcs.n_spawned(rsa) < n_max_queued_jobs ;
			return res ;
		}
		virtual Rsrcs acquire_rsrcs(RsrcsAsk const& rsa) const {
			spawned_rsrcs.inc(rsa) ;
			return rsa ;
		}
		virtual void start_rsrcs(Rsrcs const& rs) const {
			spawned_rsrcs.dec(rs) ;
		}
		virtual ::string start_job( JobIdx , SpawnedEntry const& se ) const {
			SWEAR(+se.rsrcs) ;
			return "slurm_id:"s+se.id.load() ;
		}
		virtual ::pair_s<bool/*retry*/> end_job( JobIdx j , SpawnedEntry const& se , Status s ) const {
			if ( !se.verbose && s==Status::Ok ) return {{},true/*retry*/} ;                             // common case, must be fast, if job is in error, better to ask slurm why, e.g. could be OOM
			::pair_s<Bool3/*job_ok*/> info ;
			for( int c=0 ; c<2 ; c++ ) {
				Delay d { 0.01 }                                               ;
				Pdate e = Pdate(New) + ::max(g_config->network_delay,Delay(1)) ; // ensure a reasonable minimum
				for( Pdate c = New ;; c+=d ) {
					info = slurm_job_state(se.id) ;
					if (info.second!=Maybe) goto JobDead ;
					if (c>=e              ) break        ;
					d.sleep_for() ;                                              // wait, hoping job is dying, double delay every loop until hearbeat tick
					d = ::min( d+d , g_config->heartbeat_tick ) ;
				}
				if (c==0) _s_slurm_cancel_thread.push(se.id) ;                   // if still alive after network delay, (asynchronously as faster and no return value) cancel job and retry
			}
			info.first = "job is still alive" ;
		JobDead :
			if ( se.verbose && +info.first ) {                       // XXX : only read stderr when something to say as what appears to be a filesystem bug (seen with ceph) sometimes blocks !
				::string stderr = read_stderr(j) ;
				if (+stderr) info.first <<set_nl<< stderr ;
			}
			return { info.first , info.second!=No } ;
		}
		virtual ::pair_s<HeartbeatState> heartbeat_queued_job( JobIdx j , SpawnedEntry const& se ) const {
			::pair_s<Bool3/*job_ok*/> info = slurm_job_state(se.id) ;
			if (info.second==Maybe) return {{}/*msg*/,HeartbeatState::Alive} ;
			//
			if ( se.verbose && +info.first ) {                       // XXX : only read stderr when something to say as what appears to be a filesystem bug (seen with ceph) sometimes blocks !
				::string stderr = read_stderr(j) ;
				if (+stderr) info.first <<set_nl<< stderr ;
			}
			if (info.second==Yes) return { info.first , HeartbeatState::Lost } ;
			else                  return { info.first , HeartbeatState::Err  } ;
		}
		virtual void kill_queued_job(SpawnedEntry const& se) const {
			if (!se.zombie) _s_slurm_cancel_thread.push(se.id) ;     // asynchronous (as faster and no return value) cancel
		}
		virtual SlurmId launch_job( ::stop_token st , JobIdx j , ::vector<ReqIdx> const& reqs , Pdate prio , ::vector_s const& cmd_line , Rsrcs const& rs , bool verbose ) const {
			int32_t nice = use_nice ? int32_t((prio-daemon.time_origin).sec()*daemon.nice_factor) : 0 ;
			nice &= 0x7fffffff ;                                                                         // slurm will not accept negative values, default values overflow in ... 2091
			SlurmId id = slurm_spawn_job( st , repo_key , j , reqs , nice , cmd_line , *rs , verbose ) ;
			Trace trace(BeChnl,"Slurm::launch_job",repo_key,j,id,nice,cmd_line,rs,STR(verbose)) ;
			return id ;
		}

		// data
		SpawnedMap mutable  spawned_rsrcs     ;         // number of spawned jobs queued in slurm queue
		::vector<RsrcsData> req_forces        ;         // indexed by req, resources forced by req
		uint32_t            n_max_queued_jobs = -1    ; // no limit by default
		bool                use_nice          = false ;
		::string            repo_key          ;         // a short identifier of the repository
		Daemon              daemon            ;         // info sensed from slurm daemon
	} ;

	DequeThread<SlurmId> SlurmBackend::_s_slurm_cancel_thread ;

	//
	// init
	//

	bool _inited = (SlurmBackend::s_init(),true) ;

	//
	// Daemon
	//

	::ostream& operator<<( ::ostream& os , Daemon const& d ) {
		return os << "Daemon(" << d.time_origin <<','<< d.nice_factor <<','<< d.licenses <<')' ;
	}

	//
	// RsrcsData
	//

	::ostream& operator<<( ::ostream& os , RsrcsDataSingle const& rsds ) {
		/**/                         os <<'('<< rsds.cpu       ;
		if ( rsds.mem              ) os <<','<< rsds.mem<<"MB" ;
		if ( rsds.tmp!=uint32_t(-1)) os <<','<< rsds.tmp<<"MB" ;
		if (+rsds.part             ) os <<','<< rsds.part      ;
		if (+rsds.gres             ) os <<','<< rsds.gres      ;
		if (+rsds.licenses         ) os <<','<< rsds.licenses  ;
		if (+rsds.feature          ) os <<','<< rsds.feature   ;
		if (+rsds.qos              ) os <<','<< rsds.qos       ;
		if (+rsds.reserv           ) os <<','<< rsds.reserv    ;
		if (+rsds.excludes         ) os <<','<< rsds.excludes  ;
		if (+rsds.nodes            ) os <<','<< rsds.nodes     ;
		return                       os <<')'                  ;
	}

	static void _sort_entry(::string& s) {
		if (s.find(',')==Npos) return ;
		::vector_s v = split(s,',') ;
		SWEAR(v.size()>1) ;
		sort(v) ;
		s = v[0] ;
		for( size_t i=1 ; i<v.size() ; i++ ) s<<','<<v[i] ;
	}
	inline RsrcsData::RsrcsData( ::vmap_ss&& m , Daemon d , JobIdx ji ) : Base{1} { // ensure we have at least 1 entry as we sometimes access element 0
		sort(m) ;
		for( auto&& [kn,v] : ::move(m) ) {
			size_t           p    = kn.find(':')                                           ;
			::string         k    = p==Npos ? ::move(kn) :               kn.substr(0  ,p)  ;
			uint32_t         n    = p==Npos ? 0  : from_string<uint32_t>(kn.substr(p+1  )) ;
			RsrcsDataSingle& rsds = grow(*this,n)                                          ;
			//
			auto chk_first = [&]()->void {
				if (n) throw k+" is only for 1st component of job, not component "+n ;
			} ;
			switch (k[0]) {
				case 'c' : if (k=="cpu"     ) {                                rsds.cpu      = from_string_with_units<    uint32_t>(v) ; continue ; } break ;
				case 'm' : if (k=="mem"     ) { if (d.manage_mem)              rsds.mem      = from_string_with_units<'M',uint32_t>(v) ; continue ; } break ; // dont ask mem if not managed
				case 't' : if (k=="tmp"     ) {                                rsds.tmp      = from_string_with_units<'M',uint32_t>(v) ; continue ; } break ;
				case 'e' : if (k=="excludes") {                                rsds.excludes = ::move                              (v) ; continue ; } break ;
				case 'f' : if (k=="feature" ) {                                rsds.feature  = ::move                              (v) ; continue ; } break ;
				case 'g' : if (k=="gres"    ) {               _sort_entry(v) ; rsds.gres     = ::move                              (v) ; continue ; } break ; // normalize to favor resources sharing
				case 'l' : if (k=="licenses") { chk_first() ; _sort_entry(v) ; rsds.licenses = ::move                              (v) ; continue ; } break ; // normalize to favor resources sharing
				case 'n' : if (k=="nodes"   ) {                                rsds.nodes    = ::move                              (v) ; continue ; } break ;
				case 'p' : if (k=="part"    ) {                                rsds.part     = ::move                              (v) ; continue ; } break ;
				case 'q' : if (k=="qos"     ) {                                rsds.qos      = ::move                              (v) ; continue ; } break ;
				case 'r' : if (k=="reserv"  ) {                                rsds.reserv   = ::move                              (v) ; continue ; } break ;
				default : ;
			}
			if ( auto it = d.licenses.find(k) ; it!=d.licenses.end() ) {
				chk_first() ;
				if (+rsds.licenses) rsds.licenses += ',' ;
				rsds.licenses += k+':'+v ;
				continue ;
			}
			//
			throw "no resource "+k+" for backend "+snake(MyTag) ;

		}
		if ( d.manage_mem && !(*this)[0].mem ) throw "must reserve memory when managed by slurm daemon, consider "s+Job(ji)->rule->name+".resources={'mem':'1M'}" ;
	}
	::vmap_ss RsrcsData::mk_vmap(void) const {
		::vmap_ss res ;
		// It may be interesting to know the number of cpu reserved to know how many thread to launch in some situation
		/**/                              res.emplace_back( "cpu" , to_string_with_units     ((*this)[0].cpu) ) ;
		/**/                              res.emplace_back( "mem" , to_string_with_units<'M'>((*this)[0].mem) ) ;
		if ((*this)[0].tmp!=uint32_t(-1)) res.emplace_back( "tmp" , to_string_with_units<'M'>((*this)[0].tmp) ) ;
		return res ;
	}

	RsrcsData blend( RsrcsData&& rsrcs , RsrcsData const& force ) {
		if (+force)
			for( size_t i=0 ; i<::min(rsrcs.size(),force.size()) ; i++ ) {
				RsrcsDataSingle const& force1 = force[i] ;
				if ( force1.cpu              ) rsrcs[i].cpu      = force1.cpu      ;
				if ( force1.mem              ) rsrcs[i].mem      = force1.mem      ;
				if ( force1.tmp!=uint32_t(-1)) rsrcs[i].tmp      = force1.tmp      ;
				if (+force1.excludes         ) rsrcs[i].excludes = force1.excludes ;
				if (+force1.feature          ) rsrcs[i].feature  = force1.feature  ;
				if (+force1.gres             ) rsrcs[i].gres     = force1.gres     ;
				if (+force1.licenses         ) rsrcs[i].licenses = force1.licenses ;
				if (+force1.nodes            ) rsrcs[i].nodes    = force1.nodes    ;
				if (+force1.part             ) rsrcs[i].part     = force1.part     ;
				if (+force1.qos              ) rsrcs[i].qos      = force1.qos      ;
				if (+force1.reserv           ) rsrcs[i].reserv   = force1.reserv   ;
			}
		return ::move(rsrcs) ;
	}

	//
	// slurm API
	//

	namespace SlurmApi {
		decltype(::slurm_free_ctl_conf                    )* free_ctl_conf                     = nullptr/*garbage*/ ;
		decltype(::slurm_free_job_info_msg                )* free_job_info_msg                 = nullptr/*garbage*/ ;
		decltype(::slurm_free_submit_response_response_msg)* free_submit_response_response_msg = nullptr/*garbage*/ ;
		decltype(::slurm_init                             )* init                              = nullptr/*garbage*/ ;
		decltype(::slurm_init_job_desc_msg                )* init_job_desc_msg                 = nullptr/*garbage*/ ;
		decltype(::slurm_kill_job                         )* kill_job                          = nullptr/*garbage*/ ;
		decltype(::slurm_load_ctl_conf                    )* load_ctl_conf                     = nullptr/*garbage*/ ;
		decltype(::slurm_list_append                      )* list_append                       = nullptr/*garbage*/ ;
		decltype(::slurm_list_create                      )* list_create                       = nullptr/*garbage*/ ;
		decltype(::slurm_list_destroy                     )* list_destroy                      = nullptr/*garbage*/ ;
		decltype(::slurm_load_job                         )* load_job                          = nullptr/*garbage*/ ;
		decltype(::slurm_strerror                         )* strerror                          = nullptr/*garbage*/ ;
		decltype(::slurm_submit_batch_het_job             )* submit_batch_het_job              = nullptr/*garbage*/ ;
		decltype(::slurm_submit_batch_job                 )* submit_batch_job                  = nullptr/*garbage*/ ;
	}

	static constexpr char LibSlurm[] = "libslurm.so" ;
	template<class T> void _load_func( void* handler , T*& dst , const char* name ) {
		dst = reinterpret_cast<T*>(::dlsym(handler,name)) ;
		if (!dst) throw "cannot find "s+name+" in "+LibSlurm ;
	}
	void slurm_init(const char* config_file) {
		Trace trace(BeChnl,"slurm_init",LibSlurm) ;
		void* handler = ::dlopen(LibSlurm,RTLD_NOW|RTLD_GLOBAL) ;
		if (!handler) throw "cannot find "s+LibSlurm ;
		//
		_load_func( handler , SlurmApi::free_ctl_conf                     , "slurm_free_ctl_conf"                     ) ;
		_load_func( handler , SlurmApi::free_job_info_msg                 , "slurm_free_job_info_msg"                 ) ;
		_load_func( handler , SlurmApi::free_submit_response_response_msg , "slurm_free_submit_response_response_msg" ) ;
		_load_func( handler , SlurmApi::init                              , "slurm_init"                              ) ;
		_load_func( handler , SlurmApi::init_job_desc_msg                 , "slurm_init_job_desc_msg"                 ) ;
		_load_func( handler , SlurmApi::kill_job                          , "slurm_kill_job"                          ) ;
		_load_func( handler , SlurmApi::load_ctl_conf                     , "slurm_load_ctl_conf"                     ) ;
		_load_func( handler , SlurmApi::list_append                       , "slurm_list_append"                       ) ;
		_load_func( handler , SlurmApi::list_create                       , "slurm_list_create"                       ) ;
		_load_func( handler , SlurmApi::list_destroy                      , "slurm_list_destroy"                      ) ;
		_load_func( handler , SlurmApi::load_job                          , "slurm_load_job"                          ) ;
		_load_func( handler , SlurmApi::strerror                          , "slurm_strerror"                          ) ;
		_load_func( handler , SlurmApi::submit_batch_het_job              , "slurm_submit_batch_het_job"              ) ;
		_load_func( handler , SlurmApi::submit_batch_job                  , "slurm_submit_batch_job"                  ) ;
		//
		SlurmApi::init(config_file) ;
		//
		trace("done") ;
	}

	static ::string slurm_err() {
		return SlurmApi::strerror(errno) ;
	}

	p_cxxopts create_parser() {
		p_cxxopts allocated{new cxxopts::Options("slurm","Slurm options parser for lmake")} ;
		allocated->add_options()
			( "c,cpus-per-task" , "cpus-per-task" , cxxopts::value<uint16_t>() )
			( "mem"             , "mem"           , cxxopts::value<uint32_t>() )
			( "tmp"             , "tmp"           , cxxopts::value<uint32_t>() )
			( "C,constraint"    , "constraint"    , cxxopts::value<::string>() )
			( "x,exclude"       , "exclude nodes" , cxxopts::value<::string>() )
			( "gres"            , "gres"          , cxxopts::value<::string>() )
			( "L,licenses"      , "licenses"      , cxxopts::value<::string>() )
			( "w,nodelist"      , "nodes"         , cxxopts::value<::string>() )
			( "p,partition"     , "partition"     , cxxopts::value<::string>() )
			( "q,qos"           , "qos"           , cxxopts::value<::string>() )
			( "reservation"     , "reservation"   , cxxopts::value<::string>() )
			( "h,help"          , "print usage"                                )
		;
		return allocated;
	}

	RsrcsData parse_args(::string const& args) {
		static ::string slurm = "slurm" ;                  // apparently "slurm"s.data() does not work as memory is freed right away
		Trace trace(BeChnl,"parse_args",args) ;
		//
		if (!args) return {} ;                             // fast path
		//
		::vector_s arg_vec   = split(args,' ')           ;
		uint32_t   argc      = 1                         ;
		char **    argv      = new char*[arg_vec.size()] ; // large enough to hold all args (may not be entirely used if there are several RsrcsDataSingle's)
		RsrcsData  res       ;
		bool       seen_help = false                     ;
		//
		argv[0] = slurm.data() ;
		arg_vec.push_back(":") ;                           // sentinel to parse last args
		for ( ::string& ca : arg_vec ) {
			if (ca!=":") {
				argv[argc++] = ca.data() ;
				continue ;
			}
			RsrcsDataSingle res1 ;
			try {
				auto result = g_optParse->parse(argc,argv) ;
				//
				if (result.count("cpus-per-task")) res1.cpu      = result["cpus-per-task"].as<uint16_t>() ;
				if (result.count("mem"          )) res1.mem      = result["mem"          ].as<uint32_t>() ;
				if (result.count("tmp"          )) res1.tmp      = result["tmp"          ].as<uint32_t>() ;
				if (result.count("constraint"   )) res1.feature  = result["constraint"   ].as<::string>() ;
				if (result.count("exclude"      )) res1.excludes = result["exclude"      ].as<::string>() ;
				if (result.count("gres"         )) res1.gres     = result["gres"         ].as<::string>() ;
				if (result.count("licenses"     )) res1.licenses = result["licenses"     ].as<::string>() ;
				if (result.count("nodelist"     )) res1.nodes    = result["nodelist"     ].as<::string>() ;
				if (result.count("partition"    )) res1.part     = result["partition"    ].as<::string>() ;
				if (result.count("qos"          )) res1.qos      = result["qos"          ].as<::string>() ;
				if (result.count("reservation"  )) res1.reserv   = result["reservation"  ].as<::string>() ;
				//
				if (result.count("help")) seen_help = true ;
			} catch (const cxxopts::exceptions::exception& e) {
				throw "Error while parsing slurm options: "s+e.what() ;
			}
			res.push_back(res1) ;
			argc = 1 ;
		}
		delete[] argv ;
		if (seen_help) throw g_optParse->help() ;
		return res ;
	}

	void slurm_cancel(SlurmId slurm_id) {
		//This for loop with a retry comes from the scancel Slurm utility code
		//Normally we kill mainly waiting jobs, but some "just started jobs" could be killed like that also
		//Running jobs are killed by lmake/job_exec
		Trace trace(BeChnl,"slurm_cancel",slurm_id) ;
		int i = 0/*garbage*/ ;
		Lock lock { _slurm_mutex } ;
		for( i=0 ; i<SlurmCancelTrials ; i++ ) {
			if (SlurmApi::kill_job(slurm_id,SIGKILL,KILL_FULL_JOB)==SLURM_SUCCESS) { trace("done") ; return ; }
			switch (errno) {
				case ESLURM_INVALID_JOB_ID             :
				case ESLURM_ALREADY_DONE               : trace("already_dead",errno) ;                return ;
				case ESLURM_TRANSITION_STATE_NO_UPDATE : trace("retry",i)            ; ::sleep(1+i) ; break  ;
				default : goto Bad ;
			}
		}
	Bad :
		FAIL("cannot cancel job ",slurm_id," after ",i," retries : ",slurm_err()) ;
	}

	::pair_s<Bool3/*job_ok*/> slurm_job_state(SlurmId slurm_id) {                                                         // Maybe means job has not completed
		Trace trace(BeChnl,"slurm_job_state",slurm_id) ;
		SWEAR(slurm_id) ;
		job_info_msg_t* resp = nullptr/*garbage*/ ;
		{	Lock lock { _slurm_mutex } ;
			if (SlurmApi::load_job(&resp,slurm_id,SHOW_LOCAL)!=SLURM_SUCCESS)
				switch (errno) {
					case EAGAIN                              :
					case ESLURM_ERROR_ON_DESC_TO_RECORD_COPY : //!                                             job_ok
					case ESLURM_NODES_BUSY                   : return { "slurm daemon busy : "   +slurm_err() , Maybe } ; // no info : heartbeat will retry, end will eventually cancel
					default                                  : return { "cannot load job info : "+slurm_err() , Yes   } ;
				}
		}
		::string                msg ;
		Bool3                   ok  = Yes     ;
		slurm_job_info_t const* ji  = nullptr ;
		for ( uint32_t i=0 ; i<resp->record_count ; i++ ) {
			ji = &resp->job_array[i] ;
			job_states js = job_states( ji->job_state & JOB_STATE_BASE ) ;
			switch (js) {
				// if slurm sees job failure, somthing weird occurred (if actual job fails, job_exec reports an error and completes successfully)
				// possible job_states values (from slurm.h) :
				case JOB_PENDING   :                              ok = Maybe ; continue  ;                                // queued waiting for initiation
				case JOB_RUNNING   :                              ok = Maybe ; continue  ;                                // allocated resources and executing
				case JOB_SUSPENDED :                              ok = Maybe ; continue  ;                                // allocated resources, execution suspended
				case JOB_COMPLETE  :                                           continue  ;                                // completed execution successfully
				case JOB_CANCELLED : msg = "cancelled by user"s ; ok = Yes   ; goto Done ;                                // cancelled by user
				case JOB_TIMEOUT   : msg = "timeout"s           ; ok = No    ; goto Done ;                                // terminated on reaching time limit
				case JOB_NODE_FAIL : msg = "node failure"s      ; ok = Yes   ; goto Done ;                                // terminated on node failure
				case JOB_PREEMPTED : msg = "preempted"s         ; ok = Yes   ; goto Done ;                                // terminated due to preemption
				case JOB_BOOT_FAIL : msg = "boot failure"s      ; ok = Yes   ; goto Done ;                                // terminated due to node boot failure
				case JOB_DEADLINE  : msg = "deadline reached"s  ; ok = Yes   ; goto Done ;                                // terminated on deadline
				case JOB_OOM       : msg = "out of memory"s     ; ok = No    ; goto Done ;                                // experienced out of memory error
				//   JOB_END                                                                                              // not a real state, last entry in table
				case JOB_FAILED :                                                                                         // completed execution unsuccessfully
					// when job_exec receives a signal, the bash process which launches it (which the process seen by slurm) exits with an exit code > 128
					// however, the user is interested in the received signal, not mapped bash exit code, so undo mapping
					// signaled wstatus are barely the signal number
					/**/                                      msg = "failed ("                                                                                           ;
					if      (WIFSIGNALED(ji->exit_code)     ) msg << "signal " << WTERMSIG(ji->exit_code)           <<'-'<< ::strsignal(WTERMSIG(ji->exit_code)        ) ;
					else if (!WIFEXITED(ji->exit_code)      ) msg << "??"                                                                                                ; // weird, could be a FAIL
					else if (WEXITSTATUS(ji->exit_code)>0x80) msg << "signal " << (WEXITSTATUS(ji->exit_code)-0x80) <<'-'<< ::strsignal(WEXITSTATUS(ji->exit_code)-0x80) ; // cf comment above
					else if (WEXITSTATUS(ji->exit_code)!=0  ) msg << "exit "   << WEXITSTATUS(ji->exit_code)                                                             ;
					else                                      msg << "ok"                                                                                                ;
					/**/                                      msg << ')'                                                                                                 ;
					ok = No ;
					goto Done ;
				default : FAIL("Slurm : wrong job state return for job (",slurm_id,") : ",js) ;
			}
		}
	Done :
		if ( +msg && ji->nodes ) msg << (::strchr(ji->nodes,' ')==nullptr?" on node : ":" on nodes : ") << ji->nodes ;
		SlurmApi::free_job_info_msg(resp) ;
		return { msg , ok } ;
	}

	static ::string _get_log_dir_s  (JobIdx job) { return Job(job).ancillary_file(AncillaryTag::Backend)+'/' ; }
	static ::string _get_stderr_file(JobIdx job) { return _get_log_dir_s(job) + "stderr"                     ; }
	static ::string _get_stdout_file(JobIdx job) { return _get_log_dir_s(job) + "stdout"                     ; }

	::string read_stderr(JobIdx job) {
		Trace trace(BeChnl,"Slurm::read_stderr",job) ;
		::string stderr_file = _get_stderr_file(job) ;
		try {
			::string res = read_content(stderr_file) ;
			if (!res) return {}                                    ;
			else      return "stderr from : "+stderr_file+'\n'+res ;
		} catch (::string const&) {
			return "stderr not found : "+stderr_file ;
		}
	}

	static ::string _cmd_to_string(::vector_s const& cmd_line) {
		::string res = "#!/bin/sh" ;
		char sep = '\n' ;
		for ( ::string const& s : cmd_line ) { res<<sep<<s ; sep = ' ' ; }
		res += '\n' ;
		return res ;
	}
	SlurmId slurm_spawn_job( ::stop_token st , ::string const& key , JobIdx job , ::vector<ReqIdx> const& reqs , int32_t nice , ::vector_s const& cmd_line , RsrcsData const& rsrcs , bool verbose ) {
		static char* env[1] = {const_cast<char *>("")} ;
		Trace trace(BeChnl,"slurm_spawn_job",key,job,nice,cmd_line,rsrcs,STR(verbose)) ;
		//
		SWEAR(rsrcs.size()> 0) ;
		SWEAR(nice        >=0) ;
		//
		::string                 wd          = no_slash(*g_root_dir_s)  ;
		::string                 job_name    = key + Job(job)->name()   ;
		::string                 script      = _cmd_to_string(cmd_line) ;
		::string                 stderr_file ;
		::string                 stdout_file ;
		::vector<job_desc_msg_t> job_descr   { rsrcs.size() }           ;
		::vector_s               gress       { rsrcs.size() }           ;                                                 // keep alive until slurm is called
		if(verbose) {
			stderr_file = _get_stderr_file(job) ;
			stdout_file = _get_stdout_file(job) ;
			mk_dir_s(_get_log_dir_s(job)) ;
		}
		for( uint32_t i=0 ; RsrcsDataSingle const& r : rsrcs ) {
			job_desc_msg_t* j = &job_descr[i] ;
			SlurmApi::init_job_desc_msg(j) ;
			gress[i] = "gres:"+r.gres ;
			//
			/**/                     j->env_size        = 1                                                             ;
			/**/                     j->environment     = env                                                           ;
			/**/                     j->cpus_per_task   = r.cpu                                                         ;
			/**/                     j->pn_min_memory   = r.mem                                                         ; //in MB
			if (r.tmp!=uint32_t(-1)) j->pn_min_tmp_disk = r.tmp                                                         ; //in MB
			/**/                     j->std_err         = verbose ? stderr_file.data() : const_cast<char*>("/dev/null") ;
			/**/                     j->std_out         = verbose ? stdout_file.data() : const_cast<char*>("/dev/null") ;
			/**/                     j->work_dir        = wd.data()                                                     ;
			/**/                     j->name            = const_cast<char*>(job_name.c_str())                           ;
			//
			if(+r.excludes) j->exc_nodes     = const_cast<char*>(r.excludes.data()) ;
			if(+r.feature ) j->features      = const_cast<char*>(r.feature .data()) ;
			if(+r.gres    ) j->tres_per_node =                   gress[i]  .data()  ;                                     // keep alive
			if(+r.licenses) j->licenses      = const_cast<char*>(r.licenses.data()) ;
			if(+r.nodes   ) j->req_nodes     = const_cast<char*>(r.nodes   .data()) ;
			if(+r.part    ) j->partition     = const_cast<char*>(r.part    .data()) ;
			if(+r.qos     ) j->qos           = const_cast<char*>(r.qos     .data()) ;
			if(+r.reserv  ) j->reservation   = const_cast<char*>(r.reserv  .data()) ;
			if(i==0       ) j->script        =                   script    .data()  ;
			/**/            j->nice          = NICE_OFFSET+nice                     ;
			i++ ;
		}
		for( int i=0 ; i<SlurmSpawnTrials ; i++ ) {
			submit_response_msg_t* msg = nullptr/*garbage*/ ;
			bool                   err = false  /*garbage*/ ;
			errno = 0 ;                                            // normally useless
			{	Lock lock { _slurm_mutex } ;
				if (job_descr.size()==1) {
					err = SlurmApi::submit_batch_job(&job_descr[0],&msg)!=SLURM_SUCCESS ;
				} else {
					List l = SlurmApi::list_create(nullptr) ; for ( job_desc_msg_t& c : job_descr ) SlurmApi::list_append(l,&c) ;
					err = SlurmApi::submit_batch_het_job(l,&msg)!=SLURM_SUCCESS ;
					SlurmApi::list_destroy(l) ;
				}
			}
			int sav_errno = errno ;                                // save value before calling any slurm or libc function
			if (msg) {
				SlurmId res = msg->job_id ;
				SWEAR(res!=0) ;                                    // null id is used to signal absence of id
				SlurmApi::free_submit_response_response_msg(msg) ;
				if (!sav_errno) { SWEAR(!err) ; return res ; }
			}
			SWEAR(sav_errno!=0) ;                                  // if err, we should have a errno, else if no errno, we should have had a msg containing an id
			switch (sav_errno) {
				case EAGAIN                              :
				case ESLURM_ERROR_ON_DESC_TO_RECORD_COPY :
				case ESLURM_NODES_BUSY                   : {
					trace("retry",sav_errno,SlurmApi::strerror(sav_errno)) ;
					bool zombie = true ;
					for ( Req r : reqs ) if (!r.zombie()) { zombie = false ; break ; }
					if ( zombie || !Delay(1).sleep_for(st) ) {
						trace("interrupted",i,STR(zombie)) ;
						throw "interrupted while connecting to slurm daemon"s ;
					}
				} break ;
				default : {
					::string err_str = SlurmApi::strerror(sav_errno) ;
					trace("spawn_error",sav_errno,err_str) ;
					throw "slurm spawn job error : " + err_str ;
				}
			}
		}
		trace("cannot_spawn") ;
		throw "cannot connect to slurm daemon"s ;
	}

	Daemon slurm_sense_daemon() {
		Trace trace(BeChnl,"slurm_sense_daemon") ;
		slurm_conf_t* conf = nullptr ;
		// XXX : remember last conf read so as to pass a real update_time param & optimize call
		{	Lock lock { _slurm_mutex } ;
			if (!is_target("/etc/slurm/slurm.conf")                           ) throw "no slurm config file /etc/slur/slurm.conf"s ;
			if (SlurmApi::load_ctl_conf(0/*update_time*/,&conf)!=SLURM_SUCCESS) throw "cannot reach slurm daemon : "+slurm_err()   ;
		}
		SWEAR(conf) ;
		Daemon res ;
		trace("conf",STR(conf),conf->select_type_param) ;
		res.manage_mem = conf->select_type_param&CR_MEMORY ;
		if (conf->priority_params) {
			static ::string const to_mrkr  = "time_origin=" ;
			static ::string const npd_mrkr = "nice_factor=" ;
			trace("priority_params",conf->priority_params) ;
			//
			::string spp = conf->priority_params ;
			if ( size_t pos=spp.find(to_mrkr) ; pos!=Npos ) {
				pos += to_mrkr.size() ;
				res.time_origin = Pdate(spp.substr(pos,spp.find(',',pos))) ;
			}
			//
			if ( size_t pos=spp.find(npd_mrkr) ; pos!=Npos ) {
				pos += npd_mrkr.size() ;
				res.nice_factor = from_string<float>(spp.substr(pos,spp.find(',',pos))) ;
			}
		}
		if (conf->licenses) {
			trace("licenses",conf->licenses) ;
			::vector_s rsrc_vec = split(conf->licenses,',') ;
			for( ::string const& r : rsrc_vec ) {
				size_t   p = r.find(':')                                      ;
				::string k = r.substr(0,p)                                    ;
				size_t   v = p==Npos ? 1 : from_string<size_t>(r.substr(p+1)) ;
				//
				res.licenses.emplace(k,v) ;
			}
		}
		SlurmApi::free_ctl_conf(conf) ;
		return res ;
	}

}
