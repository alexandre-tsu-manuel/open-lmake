// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "core.hh"

// XXX : rework to maintain an ordered list of waiting_queues in ReqEntry to avoid walking through all rsrcs for each launched job

// a job may have 3 states :
// - waiting : job has been submitted and is retained here until we can spawn it
// - queued  : job has been spawned but has not yet started
// - started : job has started
// spawned means queued or started

namespace Backends {

	//
	// Shared
	//

	// share actual resources data as we typically have a lot of jobs with the same resources
	template< class Data , ::unsigned_integral RefCnt > struct Shared {
		friend ostream& operator<<( ostream& os , Shared const& s ) {
			/**/           os << "Shared" ;
			if (+s) return os << *s       ;
			else    return os << "()"     ;
		}
		// static data
	private :
		static ::umap<Data,RefCnt> _s_store ;                                   // map rsrcs to refcount, always >0 (erased when reaching 0)
		// cxtors & casts
	public :
		Shared() = default ;
		//
		Shared(Shared const& r) : data{r.data} { if (data) _s_store.at(*data)++ ; }
		Shared(Shared     && r) : data{r.data} { r.data = nullptr               ; }
		//
		template<class... A> Shared( NewType , A&&... args) {
			Data d  { ::forward<A>(args)... } ;
			auto it = _s_store.find(d)        ;
			if (it==_s_store.end()) it = _s_store.insert({::move(d),1}).first ; // data is not known, create it
			else                    it->second++ ;                              // data is known, share and increment refcount
			data = &it->first ;
		}
		//
		~Shared() {
			if (!data) return ;
			auto it = _s_store.find(*data) ;
			SWEAR(it!=_s_store.end()) ;
			if (it->second==1) _s_store.erase(it) ;                             // last pointer, destroy data
			else               it->second--       ;                             // data is shared, just decrement refcount
		}
		//
		Shared& operator=(Shared s) { swap(*this,s) ; return *this ; }
		//
		bool operator==(Shared const&) const = default ;
		// access
		Data const& operator* () const { return *data   ; }
		Data const* operator->() const { return &**this ; }
		bool        operator+ () const { return data    ; }
		bool        operator! () const { return !+*this ; }
		// data
		Data const* data = nullptr ;
	} ;
	template< class Data , ::unsigned_integral RefCnt > ::umap<Data,RefCnt> Shared<Data,RefCnt>::_s_store ;
	template< class Data , ::unsigned_integral RefCnt > void swap( Shared<Data,RefCnt>& s1 , Shared<Data,RefCnt>& s2 ) {
		::swap(s1.data,s2.data) ;
	}

}

namespace std {
	template< class Data , ::unsigned_integral RefCnt > struct hash<Backends::Shared<Data,RefCnt>> {
		size_t operator()(Backends::Shared<Data,RefCnt> const& s) const {
			return hash<Data const*>()(s.data) ;
		}
	} ;
}

namespace Backends {

	template<class I> I from_string_rsrc( ::string const& k , ::string const& v ) {
		if ( k=="mem" || k=="tmp" ) return from_string_with_units<'M',I>(v) ;
		else                        return from_string_with_units<    I>(v) ;
	}

	template<class I> ::string to_string_rsrc( ::string const& k , I v ) {
		if ( k=="mem" || k=="tmp" ) return to_string_with_units<'M'>(v) ;
		else                        return to_string_with_units     (v) ;
	}

	//
	// GenericBackend
	//

	// we could maintain a list of reqs sorted by eta as we have open_req to create entries, close_req to erase them and new_req_eta to reorder them upon need
	// but this is too heavy to code and because there are few reqs and probably most of them have local jobs if there are local jobs at all, the perf gain would be marginal, if at all
	template< Tag T , class SpawnId , class RsrcsData , class RsrcsDataAsk , bool IsLocal > struct GenericBackend : Backend {

		using Rsrcs    = Shared<RsrcsData   ,JobIdx> ;
		using RsrcsAsk = Shared<RsrcsDataAsk,JobIdx> ;

		struct WaitingEntry {
			WaitingEntry() = default ;
			WaitingEntry( RsrcsAsk const& rsa , SubmitAttrs const& sa , bool v ) : rsrcs_ask{rsa} , n_reqs{1} , submit_attrs{sa} , verbose{v} {}
			// data
			RsrcsAsk    rsrcs_ask    ;
			ReqIdx      n_reqs       = 0     ; // number of reqs waiting for this job
			SubmitAttrs submit_attrs ;
			bool        verbose      = false ;
		} ;

		struct SpawnedEntry {
			void create(Rsrcs const& rs) {
				SWEAR(zombie) ;
				rsrcs   = rs    ;
				id      = 0     ;
				started = false ;
				verbose = false ;
				zombie  = false ;
			}
			Rsrcs             rsrcs   ;
			::atomic<SpawnId> id      = 0     ;
			bool              started = false ;                               // if true <=> start() has been called for this job, for assert only
			bool              verbose = false ;
			::atomic<bool   > zombie  = true  ;                               // entry waiting for suppression
		} ;
		struct SpawnedTab : ::umap<JobIdx,SpawnedEntry> {
			using Base = ::umap<JobIdx,SpawnedEntry> ;
			using typename Base::iterator       ;
			using typename Base::const_iterator ;
			//
			const_iterator find(JobIdx j) const { const_iterator res = Base::find(j) ; if ( res==Base::end() || res->second.zombie ) return Base::end() ; else return res ; }
			iterator       find(JobIdx j)       { iterator       res = Base::find(j) ; if ( res==Base::end() || res->second.zombie ) return Base::end() ; else return res ; }
			//
			size_t         size() const { return _sz ; }                      // dont trust Base::size() as zombies would be counted
			//
			iterator create( GenericBackend const& be , JobIdx j , RsrcsAsk const& rsrcs_ask ) {
				Rsrcs    rsrcs = be.acquire_rsrcs(rsrcs_ask) ;
				iterator res   = Base::try_emplace(j).first  ;
				SWEAR(res->second.zombie) ;
				res->second.create(rsrcs) ;
				_sz++ ;
				return res ;
			}
			void start( GenericBackend const& be , iterator it ) {
				SWEAR(!it->second.started) ;
				be.start_rsrcs(it->second.rsrcs) ;
				it->second.started = true ;
			}
			void erase( GenericBackend const& be , iterator it ) {
				SWEAR(!it->second.zombie) ;
				if (!it->second.started) be.start_rsrcs(it->second.rsrcs) ;
				/**/                     be.end_rsrcs  (it->second.rsrcs) ;
				if (it->second.id      ) Base::erase(it) ;
				else                     it->second.zombie = true ;           // if no id, we may not have the necesary lock to erase the entry, defer
				_sz-- ;
			} ;
			void flush(iterator it) {
				if ( it!=Base::end() && it->second.zombie ) Base::erase(it) ; // solve deferred action
			}
		private :
			size_t _sz = 0 ;                                                  // dont trust Base::size() as zombies would be counted
		} ;

		struct PressureEntry {
			// services
			bool              operator== (PressureEntry const&      ) const = default ;
			::strong_ordering operator<=>(PressureEntry const& other) const {
				if (pressure!=other.pressure) return other.pressure<=>pressure  ; // higher pressure first
				else                          return job           <=>other.job ;
			}
			// data
			CoarseDelay pressure ;
			JobIdx      job      ;
		} ;

		struct ReqEntry {
			ReqEntry() = default ;
			ReqEntry( JobIdx nj , bool v ) : n_jobs{nj} , verbose{v} {}
			// service
			void clear() {
				waiting_queues.clear() ;
				waiting_jobs  .clear() ;
				queued_jobs   .clear() ;
			}
			// data
			::umap<RsrcsAsk,set<PressureEntry>> waiting_queues ;
			::umap<JobIdx,CoarseDelay         > waiting_jobs   ;
			::uset<JobIdx                     > queued_jobs    ;         // spawned jobs until start
			JobIdx                              n_jobs         = 0     ; // manage -j option (if >0 no more than n_jobs can be launched on behalf of this req)
			bool                                verbose        = false ;
		} ;

		// specialization
		virtual void sub_config( vmap_ss const& , bool /*dynamic*/ ) {}
		//
		virtual bool call_launch_after_start() const { return false ; }
		virtual bool call_launch_after_end  () const { return false ; }
		//
		virtual bool/*ok*/   fit_eventually( RsrcsDataAsk const&          ) const { return true ; } // true if job with such resources can be spawned eventually
		virtual bool/*ok*/   fit_now       ( RsrcsAsk     const&          ) const = 0 ;             // true if job with such resources can be spawned now
		virtual Rsrcs        acquire_rsrcs ( RsrcsAsk     const&          ) const = 0 ;             // acquire maximum possible asked resources
		virtual void         start_rsrcs   ( Rsrcs        const&          ) const {}                // handle resources at start of job
		virtual void         end_rsrcs     ( Rsrcs        const&          ) const {}                // handle resources at end   of job
		virtual ::vmap_ss    export_       ( RsrcsData    const&          ) const = 0 ;             // export resources in   a publicly manageable form
		virtual RsrcsDataAsk import_       ( ::vmap_ss        && , ReqIdx ) const = 0 ;             // import resources from a publicly manageable form
		//
		virtual ::string                 start_job           ( JobIdx , SpawnedEntry const&          ) const { return  {}                        ; }
		virtual ::pair_s<bool/*retry*/>  end_job             ( JobIdx , SpawnedEntry const& , Status ) const { return {{},false/*retry*/       } ; }
		virtual ::pair_s<HeartbeatState> heartbeat_queued_job( JobIdx , SpawnedEntry const&          ) const { return {{},HeartbeatState::Alive} ; } // only called before start
		virtual void                     kill_queued_job     (          SpawnedEntry const&          ) const = 0 ;                                   // .
		//
		virtual SpawnId launch_job( ::stop_token , JobIdx , ::vector<ReqIdx> const& , Pdate prio , ::vector_s const& cmd_line , Rsrcs const& , bool verbose ) const = 0 ;

		// services
		virtual void config( vmap_ss const& dct , bool dynamic ) {
			sub_config(dct,dynamic) ;
			_launch_queue.open( 'L' , [&](::stop_token st)->void { _launch(st) ; } ) ;
		}
		virtual bool is_local() const {
			return IsLocal ;
		}
		virtual void open_req( ReqIdx req , JobIdx n_jobs ) {
			Trace trace(BeChnl,"open_req",req,n_jobs) ;
			Lock lock     { Req::s_reqs_mutex }                                                              ;               // taking Req::s_reqs_mutex is compulsery to derefence req
			bool inserted = reqs.insert({ req , {n_jobs,Req(req)->options.flags[ReqFlag::Verbose]} }).second ;
			SWEAR(inserted) ;
		}
		virtual void close_req(ReqIdx req) {
			auto it = reqs.find(req) ;
			Trace trace(BeChnl,"close_req",req,STR(it==reqs.end())) ;
			if (it==reqs.end()) return ;                                                                                     // req has been killed
			ReqEntry const& re = it->second ;
			SWEAR(!re.waiting_jobs) ;
			SWEAR(!re.queued_jobs ) ;
			reqs.erase(it) ;
			if (!reqs) {
				SWEAR(!waiting_jobs       ) ;
				SWEAR(!spawned_jobs.size()) ;                                                                                // there may be zombie entries waiting for destruction
			}
		}
		// do not launch immediately to have a better view of which job should be launched first
		virtual void submit( JobIdx job , ReqIdx req , SubmitAttrs const& submit_attrs , ::vmap_ss&& rsrcs ) {
			RsrcsAsk rsa { New , import_(::move(rsrcs),req) } ;                                                              // compile rsrcs
			if (!fit_eventually(*rsa)) throw to_string("not enough resources to launch job ",Job(job)->name()) ;
			ReqEntry& re = reqs.at(req) ;
			SWEAR(!waiting_jobs   .contains(job)) ;                                                                          // job must be a new one
			SWEAR(!re.waiting_jobs.contains(job)) ;                                                                          // in particular for this req
			CoarseDelay pressure = submit_attrs.pressure ;
			Trace trace(BeChnl,"submit",rsa,pressure) ;
			//
			re.waiting_jobs[job] = pressure ;
			waiting_jobs.emplace( job , WaitingEntry(rsa,submit_attrs,re.verbose) ) ;
			re.waiting_queues[rsa].insert({pressure,job}) ;
			_new_submitted_jobs = true ;                                                                                     // called from main thread, as launch
		}
		virtual void add_pressure( JobIdx job , ReqIdx req , SubmitAttrs const& submit_attrs ) {
			Trace trace(BeChnl,"add_pressure",job,req,submit_attrs) ;
			ReqEntry& re  = reqs.at(req)           ;
			auto      wit = waiting_jobs.find(job) ;
			if (wit==waiting_jobs.end()) {                                                                                   // job is not waiting anymore, mostly ignore
				auto sit = spawned_jobs.find(job) ;
				if (sit==spawned_jobs.end()) {                                                                               // job is already ended
					trace("ended") ;
				} else {
					SpawnedEntry& se = sit->second ;                                                                         // if not waiting, it must be spawned if add_pressure is called
					if (re.verbose ) se.verbose = true ;                                                                     // mark it verbose, though
					trace("queued") ;
				}
				return ;
			}
			WaitingEntry& we = wit->second ;
			SWEAR(!re.waiting_jobs.contains(job)) ;                                                                          // job must be new for this req
			CoarseDelay pressure = submit_attrs.pressure ;
			trace("adjusted_pressure",pressure) ;
			//
			re.waiting_jobs[job] = pressure ;
			re.waiting_queues[we.rsrcs_ask].insert({pressure,job}) ;                                                         // job must be known
			we.submit_attrs |= submit_attrs ;
			we.verbose      |= re.verbose   ;
			we.n_reqs++ ;
		}
		virtual void set_pressure( JobIdx job , ReqIdx req , SubmitAttrs const& submit_attrs ) {
			ReqEntry& re = reqs.at(req)           ;                                                                          // req must be known to already know job
			auto      it = waiting_jobs.find(job) ;
			//
			if (it==waiting_jobs.end()) return ;                                                                             // job is not waiting anymore, ignore
			WaitingEntry        & we           = it->second                         ;
			CoarseDelay         & old_pressure = re.waiting_jobs  .at(job         ) ;                                        // job must be known
			::set<PressureEntry>& q            = re.waiting_queues.at(we.rsrcs_ask) ;                                        // including for this req
			CoarseDelay           pressure     = submit_attrs.pressure              ;
			Trace trace("set_pressure","pressure",pressure) ;
			we.submit_attrs |= submit_attrs ;
			q.erase ({old_pressure,job}) ;
			q.insert({pressure    ,job}) ;
			old_pressure = pressure ;
		}
	protected :
		virtual ::string start(JobIdx job) {
			auto          it = spawned_jobs.find(job) ; if (it==spawned_jobs.end()) return {} ;                              // job was killed in the mean time
			SpawnedEntry& se = it->second             ;
			//
			spawned_jobs.start(*this,it) ;
			for( auto& [r,re] : reqs ) re.queued_jobs.erase(job) ;
			::string msg = start_job(job,se) ;
			if (call_launch_after_start()) _launch_queue.wakeup() ;                                                          // not compulsery but improves reactivity
			return msg ;
		}
		virtual ::pair_s<bool/*retry*/> end( JobIdx j , Status s ) {
			auto                    it     = spawned_jobs.find(j) ; if (it==spawned_jobs.end()) return {{},false/*retry*/} ; // job was killed in the mean time
			SpawnedEntry&           se     = it->second           ; SWEAR(se.started) ;
			::pair_s<bool/*retry*/> digest = end_job(j,se,s)      ;
			spawned_jobs.erase(*this,it) ;                                                                                   // erase before calling launch so job is freed w.r.t. n_jobs
			if (call_launch_after_end()) _launch_queue.wakeup() ;                                                            // not compulsery but improves reactivity
			return digest ;
		}
		virtual ::pair_s<HeartbeatState> heartbeat(JobIdx j) {                                                      // called on jobs that did not start after at least newwork_delay time
			{	auto          it = spawned_jobs.find(j) ; if (it==spawned_jobs.end()) goto NotLaunched ;
				SpawnedEntry& se = it->second           ;
				SWEAR(!se.started,j) ;                                                                              // we should not be called on started jobs
				if (!se.id) {
					Lock lock { id_mutex } ;
					if (!se.id) goto NotLaunched ;                                                                  // repeat test so test and decision are atomic
				}
				::pair_s<HeartbeatState> digest = heartbeat_queued_job(j,se) ;
				if (digest.second!=HeartbeatState::Alive) {
					Trace trace(BeChnl,"heartbeat",j,se.id,digest.second) ;
					spawned_jobs.erase(*this,it) ;
					for( auto& [r,re] : reqs ) re.queued_jobs.erase(j ) ;
				}
				return digest ;
			}
		NotLaunched :
			return {"could not launch job",HeartbeatState::Err} ;
		}
		// kill all if req==0
		virtual ::vector<JobIdx> kill_waiting_jobs(ReqIdx req=0) {
			::vector<JobIdx> res ;
			Trace trace(BeChnl,"kill_req",T,req,reqs.size()) ;
			if ( !req || reqs.size()<=1 ) {
				if (req) SWEAR( reqs.size()==1 && req==reqs.begin()->first , req , reqs.size() ) ;               // ensure the last req is the right one
				// kill waiting jobs
				for( auto const& [j,_] : waiting_jobs ) res.push_back(j) ;
				waiting_jobs.clear() ;
				for( auto& [_,re] : reqs ) re.clear() ;
			} else {
				auto      rit = reqs.find(req) ; SWEAR(rit!=reqs.end()) ;                                           // we should not kill a non-existent req
				ReqEntry& re  = rit->second    ;
				// kill waiting jobs
				for( auto const& [j,_] : re.waiting_jobs ) {
					WaitingEntry& we = waiting_jobs.at(j) ;
					if (we.n_reqs==1) waiting_jobs.erase(j) ;
					else              we.n_reqs--           ;
					res.push_back(j) ;
				}
				re.clear() ;
			}
			return res ;
		}
		virtual void kill_job(JobIdx j) {
			Trace trace(BeChnl,"kill_job",j) ;
			auto          it = spawned_jobs.find(j) ;
			SpawnedEntry& se = it->second           ;
			SWEAR(!se.started) ;                                                                                    // if job is started, it is not our responsibility any more
			if (se.id) {                                     kill_queued_job(se) ; spawned_jobs.erase(*this,it) ; }
			else       { Lock lock { id_mutex } ; if (se.id) kill_queued_job(se) ; spawned_jobs.erase(*this,it) ; } // lock to ensure se.id is up to date and do same actions (erase while holding lock)
		}
		virtual void launch() {
			if (!_new_submitted_jobs) return ;
			_new_submitted_jobs = false ;                                                                           // called from main thread, as submit
			_launch_queue.wakeup() ;
		}
		void _launch(::stop_token st) {
			struct LaunchDescr {
				::vector<ReqIdx>   reqs     ;
				::vector_s         cmd_line ;
				Pdate              prio     ;
				SpawnedEntry*      entry    = nullptr ;
			} ;
			for( auto [req,eta] : Req::s_etas() ) {                                                                 // /!\ it is forbidden to dereference req without taking Req::s_reqs_mutex first
				Trace trace(BeChnl,"launch",req) ;
				::vmap<JobIdx,LaunchDescr> launch_descrs ;
				{	Lock lock { _s_mutex } ;
					auto rit = reqs.find(+req) ;
					if (rit==reqs.end()) continue ;
					JobIdx                               n_jobs = rit->second.n_jobs         ;
					::umap<RsrcsAsk,set<PressureEntry>>& queues = rit->second.waiting_queues ;
trace("spawned_jobs1",spawned_jobs.size(),spawned_jobs.Base::size(),n_jobs) ;
::umap<RsrcsAsk,size_t> q_szs ; for( auto const& [rsa,ps] : queues ) q_szs[rsa] = ps.size() ;
trace("queue_sizes",q_szs) ;
					for(;;) {
						if ( n_jobs && spawned_jobs.size()>=n_jobs ) break ;                                        // cannot have more than n_jobs running jobs because of this req, process next req
						auto candidate = queues.end() ;
						for( auto it=queues.begin() ; it!=queues.end() ; it++ ) {
							if ( candidate!=queues.end() && it->second.begin()->pressure<=candidate->second.begin()->pressure ) continue ;
							if (fit_now(it->first)) candidate = it ;                                                                       // continue to find a better candidate
						}
						if (candidate==queues.end()) break ;                                                                               // nothing for this req, process next req
						//
						::set<PressureEntry>& pressure_set = candidate->second                                     ;
						auto                  pressure1    = pressure_set.begin()                                  ; SWEAR(pressure1!=pressure_set.end(),candidate->first) ; // a candidate ...
						Pdate                 prio         = eta-pressure1->pressure                               ;                                                         // ... with no pressure ?
						JobIdx                j            = pressure1->job                                        ;
						auto                  wit          = waiting_jobs.find(j)                                  ;
						SpawnedEntry&         se           = spawned_jobs.create(*this,j,candidate->first)->second ;
						//
						se.verbose = wit->second.verbose ;
						::vector<ReqIdx> rs { +req } ;
						for( auto const& [r,re] : reqs )
							if      (!re.waiting_jobs.contains(j)) SWEAR(r!=+req,r) ;
							else if (r!=+req                     ) rs.push_back(r)  ;
						launch_descrs.emplace_back( j , LaunchDescr{ rs , acquire_cmd_line(T,j,rs,export_(*se.rsrcs),wit->second.submit_attrs) , prio , &se } ) ;
						waiting_jobs.erase(wit) ;
						//
						for( ReqIdx r : rs ) {
							ReqEntry& re   = reqs.at(r)              ;
							auto      wit1 = re.waiting_jobs.find(j) ;
							if (r!=+req) {
								auto                  wit2 = re.waiting_queues.find(candidate->first) ;
								::set<PressureEntry>& pes  = wit2->second                             ;
								PressureEntry         pe   { wit1->second , j }                       ;                // /!\ pressure is job pressure for r, not for req
								SWEAR(pes.contains(pe)) ;
								if (pes.size()==1) re.waiting_queues.erase(wit2) ;                                     // last entry for this rsrcs, erase the entire queue
								else               pes              .erase(pe  ) ;
							}
							re.waiting_jobs.erase (wit1) ;
							re.queued_jobs .insert(j   ) ;
						}
						if (pressure_set.size()==1) queues      .erase(candidate) ;                                    // last entry for this rsrcs, erase the entire queue
						else                        pressure_set.erase(pressure1) ;
					}
				}
				for( auto& [ji,ld] : launch_descrs ) {
					Lock lock { id_mutex } ;
					SpawnedEntry& se = *ld.entry ;
					if (se.zombie) continue ;                                                                          // job was cancelled before being launched
					try {
						SpawnId id = launch_job( st , ji , ld.reqs , ld.prio , ld.cmd_line , se.rsrcs , se.verbose ) ; // XXX : manage errors, for now rely on heartbeat
						SWEAR(id,ji) ;                                                                                 // null id is used to mark absence of id
						se.id = id ;
						trace("child",ji,ld.prio,id,ld.cmd_line) ;
					} catch (::string const& e) {
						trace("fail",ji,ld.prio,e) ;
					}
				}
				{	Lock lock { _s_mutex } ;
					for( auto const& [ji,_] : launch_descrs ) {
						auto it=spawned_jobs.find(ji) ; if (it==spawned_jobs.end()) continue ;
						if (!it->second.id) spawned_jobs.erase(*this,it) ;                                             // job could not be launched, release resources
						/**/                spawned_jobs.flush(      it) ;                                             // collect unused entry now that we hold _s_mutex
					}
					launch_descrs.clear() ;                                                                            // destroy entries while holding the lock
				}
trace("spawned_jobs2",spawned_jobs.size(),spawned_jobs.Base::size()) ;
				trace("done") ;
			}
		}

		// data
		::umap<ReqIdx,ReqEntry    > reqs         ;                         // all open Req's
		::umap<JobIdx,WaitingEntry> waiting_jobs ;                         // jobs retained here
		SpawnedTab                  spawned_jobs ;                         // jobs spawned until end
	protected :
		Mutex<MutexLvl::BackendId> mutable id_mutex ;
	private :
		WakeupThread<false/*Flush*/> mutable _launch_queue       ;
		bool                                 _new_submitted_jobs = false ; // submit and launch are both called from main thread, so no need for protection

	} ;

}
