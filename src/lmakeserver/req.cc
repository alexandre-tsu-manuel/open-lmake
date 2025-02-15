// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "core.hh"

#include <stdexcept>

using namespace Disk ;
using namespace Time ;

namespace Engine {

	//
	// Req
	//

	SmallIds<ReqIdx,true/*ThreadSafe*/>         Req::s_small_ids     ;
	::vector<Req>                               Req::s_reqs_by_start ;
	Mutex<MutexLvl::Req>                        Req::s_reqs_mutex    ;
	::vector<ReqData>                           Req::s_store(1)      ;
	::vector<Req>                               Req::_s_reqs_by_eta  ;
	::array<atomic<bool>,1<<(sizeof(ReqIdx)*8)> Req::_s_zombie_tab   = { true } ; // Req 0 is zombie, all other ones are not

	::ostream& operator<<( ::ostream& os , Req const r ) {
		return os << "Rq(" << int(+r) << ')' ;
	}

	void Req::make(EngineClosureReq const& ecr) {
		SWEAR(s_store.size()>+*this) ;                                                           // ensure data exist
		ReqData& data = **this ;
		//
		for( int i=0 ;; i++ ) {                                                                  // try increasing resolution in file name until no conflict
			::string lcl_log_file = "outputs/"+Pdate(New).str(i) ;
			::string log_file     = AdminDirS+lcl_log_file       ;
			if (FileInfo(log_file).tag()>=FileTag::Reg) { SWEAR(i<=9,i) ; continue ; }           // if conflict, try higher resolution, at ns resolution, it impossible to have a conflict
			//
			::string last = AdminDirS+"last_output"s ;
			//
			data.log_stream.open(log_file) ;
			try         { unlnk(last) ; lnk(last,lcl_log_file) ;                               }
			catch (...) { exit(Rc::System,"cannot create symlink ",last," to ",lcl_log_file) ; }
			data.start_ddate = file_date(log_file) ;                                             // use log_file as a date marker
			data.start_pdate = New                 ;
			break ;
		}
		//
		data.eta          = data.start_pdate   ;
		data.idx_by_start = s_n_reqs()         ;
		data.idx_by_eta   = s_n_reqs()         ;                                                 // initially, eta is far future
		data.jobs .dflt   = JobReqInfo (*this) ;
		data.nodes.dflt   = NodeReqInfo(*this) ;
		data.options      = ecr.options        ;
		data.audit_fd     = ecr.out_fd         ;
		//
		s_reqs_by_start.push_back(*this) ;
		_adjust_eta(true/*push_self*/) ;
		//
		Trace trace("Rmake",*this,s_n_reqs(),data.start_ddate,data.start_pdate) ;
		try {
			JobIdx n_jobs = from_string<JobIdx>(ecr.options.flag_args[+ReqFlag::Jobs],true/*empty_ok*/) ;
			if (ecr.as_job()) data.job = ecr.job()                                                                            ;
			else              data.job = Job( Special::Req , Deps(ecr.targets(),~Accesses(),Dflag::Static,true/*parallel*/) ) ;
			Backend::s_open_req( +*this , n_jobs ) ;
			data.has_backend = true ;
		} catch (::string const& e) {
			close() ;
			throw ;
		}
		trace("job",data.job) ;
		//
		Job::ReqInfo& jri = data.job->req_info(*this) ;
		jri.live_out = (*this)->options.flags[ReqFlag::LiveOut] ;
		//vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
		data.job->make(jri,JobMakeAction::Status,{}/*JobReason*/,No/*speculate*/) ;
		//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
		for( Node d : data.job->deps ) {
			/**/                           if (!d->done(*this)             ) continue ;
			Job j = d->conform_job_tgt() ; if (!j                          ) continue ;
			/**/                           if (j->run_status!=RunStatus::Ok) continue ;
			//
			(*this)->up_to_dates.push_back(d) ;
		}
		chk_end() ;
	}

	void Req::kill() {
		Trace trace("kill",*this) ;
		SWEAR(zombie()) ;                                                            // zombie has already been set
		audit_ctrl_c( (*this)->audit_fd , (*this)->log_stream , (*this)->options ) ;
		Backend::s_kill_req(+*this) ;
	}

	void Req::close() {
		Trace trace("close",*this) ;
		SWEAR(  (*this)->is_open  ()                        ) ;
		SWEAR( !(*this)->n_running() , (*this)->n_running() ) ;
		if ((*this)->has_backend) Backend::s_close_req(+*this) ;
		// erase req from sorted vectors by physically shifting reqs that are after
		Idx n_reqs = s_n_reqs() ;
		for( Idx i=(*this)->idx_by_start ; i<n_reqs-1 ; i++ ) {
			s_reqs_by_start[i]               = s_reqs_by_start[i+1] ;
			s_reqs_by_start[i]->idx_by_start = i                    ;
		}
		s_reqs_by_start.pop_back() ;
		{	Lock lock{s_reqs_mutex} ;
			for( Idx i=(*this)->idx_by_eta ; i<n_reqs-1 ; i++ ) {
				_s_reqs_by_eta[i]             = _s_reqs_by_eta[i+1] ;
				_s_reqs_by_eta[i]->idx_by_eta = i                   ;
			}
			_s_reqs_by_eta.pop_back() ;
		}
	}

	void Req::new_eta() {
		Pdate new_eta   = Backend::s_submitted_eta(*this) + (*this)->stats.waiting_cost ;
		Pdate old_eta   = (*this)->eta                                                  ;
		Delay old_ete   = old_eta-Pdate(New)                                            ;
		Delay delta_ete = new_eta>old_eta ? new_eta-old_eta : old_eta-new_eta           ; // cant use ::abs(new_eta-old_eta) because of signedness
		//
		if ( delta_ete.val() <= (old_ete.val()>>4) ) return ;                             // eta did not change significatively
		(*this)->eta = new_eta ;
		_adjust_eta() ;
		Backend::s_new_req_etas() ;                                                       // tell backends that etas changed significatively
	}

	void Req::_adjust_eta(bool push_self) {
			Trace trace("_adjust_eta",(*this)->eta) ;
			// reorder _s_reqs_by_eta and adjust idx_by_eta to reflect new order
			bool changed    = false               ;
			Lock lock       { s_reqs_mutex }      ;
			Idx  idx_by_eta = (*this)->idx_by_eta ;
			if (push_self) _s_reqs_by_eta.push_back(*this) ;
			while ( idx_by_eta>0 && _s_reqs_by_eta[idx_by_eta-1]->eta>(*this)->eta ) {                       // if eta is decreased
				( _s_reqs_by_eta[idx_by_eta  ] = _s_reqs_by_eta[idx_by_eta-1] )->idx_by_eta = idx_by_eta   ; // swap w/ prev entry
				( _s_reqs_by_eta[idx_by_eta-1] = *this                        )->idx_by_eta = idx_by_eta-1 ; // .
				changed = true ;
			}
			if (changed) return ;
			while ( idx_by_eta+1<s_n_reqs() && _s_reqs_by_eta[idx_by_eta+1]->eta<(*this)->eta ) {            // if eta is increased
				( _s_reqs_by_eta[idx_by_eta  ] = _s_reqs_by_eta[idx_by_eta+1] )->idx_by_eta = idx_by_eta   ; // swap w/ next entry
				( _s_reqs_by_eta[idx_by_eta+1] = *this                        )->idx_by_eta = idx_by_eta+1 ; // .
			}
	}

	void Req::_report_cycle(Node node) {
		::uset  <Node> seen      ;
		::vector<Node> cycle     ;
		::uset  <Rule> to_raise  ;
		::vector<Node> to_forget ;
		for( Node d=node ; seen.insert(d).second ;) {
			NodeStatus dns = d->status() ;
			if ( dns!=NodeStatus::Unknown && dns>=NodeStatus::Uphill ) {
				d = d->dir() ;
				goto Next ;
			}
			for( Job j : d->conform_job_tgts(d->c_req_info(*this)) )        // 1st pass to find done rules which we suggest to raise the prio of to avoid the loop
				if (j->c_req_info(*this).done()) to_raise.insert(j->rule) ;
			for( Job j : d->conform_job_tgts(d->c_req_info(*this)) ) {      // 2nd pass to find the loop
				JobReqInfo const& cjri = j->c_req_info(*this) ;
				if (cjri.done()          ) continue ;
				if (cjri.speculative_wait) to_forget.push_back(d) ;
				for( Node dd : j->deps ) {
					if (dd->done(*this)) continue ;
					d = dd ;
					goto Next ;
				}
				fail_prod("not done but all deps are done :",j->name()) ;
			}
			fail_prod("not done but all pertinent jobs are done :",d->name()) ;
		Next :
			cycle.push_back(d) ;
		}
		(*this)->audit_node( Color::Err , "cycle detected for",node ) ;
		Node deepest   = cycle.back()  ;
		bool seen_loop = deepest==node ;
		for( size_t i=0 ; i<cycle.size() ; i++ ) {
			const char* prefix ;
			/**/ if ( seen_loop && i==0 && i==cycle.size()-1 ) { prefix = "^-- " ;                    }
			else if ( seen_loop && i==0                      ) { prefix = "^   " ;                    }
			else if (                      i==cycle.size()-1 ) { prefix = "+-- " ;                    }
			else if ( seen_loop && i!=0                      ) { prefix = "|   " ;                    }
			else if ( cycle[i]==deepest                      ) { prefix = "+-> " ; seen_loop = true ; }
			else                                               { prefix = "    " ;                    }
			(*this)->audit_node( Color::Note , prefix,cycle[i] , 1 ) ;
		}
		if ( +to_forget || +to_raise ) {
			(*this)->audit_info( Color::Note , "consider some of :\n" ) ;
			for( Node n : to_forget ) (*this)->audit_node( Color::Note , "lforget -d" , n                             , 1 ) ;
			if (+to_raise)            (*this)->audit_info( Color::Note , "add to Lmakefile.py :"                      , 1 ) ;
			for( Rule r : to_raise  ) (*this)->audit_info( Color::Note , r->name+".prio = "+::to_string(r->prio)+"+1" , 2 ) ;
			/**/                      (*this)->audit_info( Color::Note , "for t in (<cycle above>) :"                 , 2 ) ;
			/**/                      (*this)->audit_info( Color::Note , "class MyAntiRule(AntiRule) :"               , 3 ) ;
			/**/                      (*this)->audit_info( Color::Note , "target = t"                                 , 4 ) ;
		}
	}

	bool/*overflow*/ Req::_report_err( Dep const& dep , size_t& n_err , bool& seen_stderr , ::uset<Job>& seen_jobs , ::uset<Node>& seen_nodes , DepDepth lvl ) {
		if (dep.dflags[Dflag::IgnoreError]) return false/*overflow*/ ;
		if (seen_nodes.contains(dep)      ) return false/*overflow*/ ;
		seen_nodes.insert(dep) ;
		NodeReqInfo const& cri = dep->c_req_info(*this) ;
		const char*        err = nullptr                ;
		switch (dep->status()) {
			case NodeStatus::Multi     :                                  err = "multi"                                        ; break ;
			case NodeStatus::Transient :                                  err = "missing transient sub-file"                   ; break ;
			case NodeStatus::Uphill    : if (dep.dflags[Dflag::Required]) err = "missing required sub-file"                    ; break ;
			case NodeStatus::Src       : if (dep->crc==Crc::None        ) err = dep.frozen()?"missing frozen":"missing source" ; break ;
			case NodeStatus::SrcDir    : if (dep.dflags[Dflag::Required]) err = "missing required"                             ; break ;
			case NodeStatus::Plain :
				if (+cri.overwritten)
					err = "overwritten" ;
				else if (+dep->conform_job_tgts(cri))
					for( Job job : dep->conform_job_tgts(cri) ) {
						if (_report_err( job , dep , n_err , seen_stderr , seen_jobs , seen_nodes , lvl )) return true/*overflow*/ ;
					}
				else
					err = "not built" ; // if no better explanation found
			break ;
			case NodeStatus::None :
				if      (dep->manual()>=Manual::Changed) err = "dangling" ;
				else if (dep.dflags[Dflag::Required]   ) err = "missing"  ;
			break ;
		DF}
		if (err) return (*this)->_send_err( false/*intermediate*/ , err , dep->name() , n_err , lvl ) ;
		else     return false/*overflow*/                                                             ;
	}

	bool/*overflow*/ Req::_report_err( Job job , Node target , size_t& n_err , bool& seen_stderr , ::uset<Job>& seen_jobs , ::uset<Node>& seen_nodes , DepDepth lvl ) {
		if (seen_jobs.contains(job)) return false/*overflow*/ ;
		seen_jobs.insert(job) ;
		JobReqInfo const& jri = job->c_req_info(*this) ;
		if (!jri.done()) return false/*overflow*/ ;
		if (!job->err()) return false/*overflow*/ ;
		//
		bool intermediate = job->run_status==RunStatus::DepErr ;
		if ( (*this)->_send_err( intermediate , job->rule->name , +target?target->name():job->name() , n_err , lvl ) ) return true/*overflow*/ ;
		//
		if ( !seen_stderr && job->run_status==RunStatus::Ok ) // show first stderr
			switch (job->rule->special) {
				case Special::Infinite :
					(*this)->audit_info( Color::None , job->special_stderr() , lvl+1 ) ;
					seen_stderr = true ;
				break ;
				case Special::Plain : {
					Rule::SimpleMatch match          ;
					JobInfo           job_info       = job.job_info()                                                       ;
					EndNoneAttrs      end_none_attrs = job->rule->end_none_attrs.eval( job , match , job_info.start.rsrcs ) ;
					//
					if (!job_info.end.end.proc) (*this)->audit_info( Color::Note , "no stderr available" , lvl+1 ) ;
					else                        seen_stderr = (*this)->audit_stderr( job , job_info.end.end.msg , job_info.end.end.digest.stderr , end_none_attrs.max_stderr_len , lvl+1 ) ;
				} break ;
			DN}
		if (intermediate)
			for( Dep const& d : job->deps )
				if ( _report_err( d , n_err , seen_stderr , seen_jobs , seen_nodes , lvl+1 ) ) return true/*overflow*/ ;
		return false/*overflow*/ ;
	}

	void Req::chk_end() {
		if ((*this)->n_running()) return ;
		Job               job     = (*this)->job            ;
		JobReqInfo const& cri     = job->c_req_info(*this)  ;
		bool              job_err = job->status!=Status::Ok ;
		Trace trace("chk_end",*this,cri,job,job->status) ;
		(*this)->audit_stats() ;
		(*this)->audit_summary(job_err) ;
		if (zombie()                      ) goto Done ;
		if (!job_err                      ) goto Done ;
		if (!job->c_req_info(*this).done()) {
			for( Dep const& d : job->deps )
				if (!d->done(*this)) { _report_cycle(d) ; goto Done ; }
			fail_prod("job not done but all deps are done :",job->name()) ;
		} else {
			size_t       n_err       = g_config->max_err_lines ? g_config->max_err_lines : size_t(-1) ;
			bool         seen_stderr = false                                                          ;
			::uset<Job > seen_jobs   ;
			::uset<Node> seen_nodes  ;
			NfsGuard     nfs_guard   { g_config->reliable_dirs }                                      ;
			if (job->rule->special==Special::Req) {
				for( Dep const& d : job->deps ) if (d->status()<=NodeStatus::Makable) _report_err             (d     ,n_err,seen_stderr,seen_jobs,seen_nodes) ;
				for( Dep const& d : job->deps ) if (d->status()> NodeStatus::Makable) (*this)->_report_no_rule(d,nfs_guard                                  ) ;
			} else {
				/**/                                                                  _report_err             (job,{},n_err,seen_stderr,seen_jobs,seen_nodes) ;
			}
		}
	Done :
		(*this)->audit_status(!job_err) ;
		(*this)->audit_fd.detach() ;      // ensure we send nothing anymore
		//vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
		g_engine_queue.emplace(ReqProc::Close,*this) ;
		//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
	} ;

	//
	// ReqInfo
	//

	::ostream& operator<<( ::ostream& os , ReqInfo const& ri ) {
		return os<<"ReqInfo("<<ri.req<<",W:"<<ri.n_wait<<"->"<<ri.n_watchers()<<')' ;
	}

	void ReqInfo::_add_watcher(Watcher watcher) {
		switch (_n_watchers) {
			//                vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
			case VectorMrkr : _watchers_v->emplace_back( watcher) ;                 break ; // vector stays vector , simple
			default         : _watchers_a[_n_watchers] = watcher  ; _n_watchers++ ; break ; // array stays array   , simple
			//                ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
			case NWatchers :                                                                // array becomes vector, complex
				::array<Watcher,NWatchers> tmp = _watchers_a ;
				_watchers_a.~array() ;
				::vector<Watcher>& ws = *new ::vector<Watcher>(NWatchers+1) ;               // cannot put {} here or it is taken as an initializer list
				for( uint8_t i=0 ; i<NWatchers ; i++ ) ws[i] = tmp[i] ;
				//vvvvvvvvvvvvvvvvvvvvv
				ws[NWatchers] = watcher ;
				//^^^^^^^^^^^^^^^^^^^^^
				_watchers_v = &ws        ;
				_n_watchers = VectorMrkr ;
		}
	}

	void ReqInfo::wakeup_watchers() {
		SWEAR(!waiting()) ;                                                         // dont wake up watchers if we are not ready
		::vector<Watcher> watchers ;                                                // copy watchers aside before calling them as during a call, we could become not done and be waited for again
		if (_n_watchers==VectorMrkr) {
			watchers = ::move(*_watchers_v) ;
			delete _watchers_v ;                                                    // transform vector into array as there is no watchers any more
		} else {
			watchers = mk_vector(::vector_view(_watchers_a.data(),_n_watchers)) ;
		}
		_n_watchers = 0 ;
		// we are done for a given RunAction, but calling make on a dependent may raise the RunAciton and we can become waiting() again
		for( auto it = watchers.begin() ; it!=watchers.end() ; it++ )
			if      (waiting()      ) _add_watcher(*it) ;                           // if waiting again, add back watchers we have got and that we no more want to call
			else if (it->is_a<Job>()) Job (*it)->wakeup(Job (*it)->req_info(req)) ; // ok, we are still done, we can call watcher
			else                      Node(*it)->wakeup(Node(*it)->req_info(req)) ; // .
	}

	Job ReqInfo::asking() const {
		::c_vector_view<Watcher> watchers{
			_n_watchers==VectorMrkr ? _watchers_v->data() : _watchers_a.data()
		,	_n_watchers==VectorMrkr ? _watchers_v->size() : _n_watchers
		} ;
		for( Watcher w : watchers )
			if (w.is_a<Job>()) { Job j =      w                            ; if (!j->is_special()) return j ; }
			else               { Job j = Node(w)->c_req_info(req).asking() ; if (+j              ) return j ; }
		return {} ;
	}

	//
	// ReqData
	//

	Mutex<MutexLvl::Audit> ReqData::_s_audit_mutex ;

	void ReqData::clear() {
		Trace trace("clear",job) ;
		SWEAR( !n_running() , n_running() ) ;
		if ( +job && job->rule->special==Special::Req ) job.pop();
		*this = {} ;
	}

	void ReqData::audit_summary(bool err) const {
		bool warning = +frozen_jobs || +no_triggers || +clash_nodes ;
		audit_info( err ? Color::Err : warning ? Color::Warning : Color::Note ,
			"+---------+\n"
			"| SUMMARY |\n"
			"+---------+\n"
		) ;
		size_t wk = ::max("elapsed"s.size(),"startup"s.size()) ;
		size_t wn = 0                                          ;
		for( JobReport jr : All<JobReport> ) if ( stats.ended[+jr] || jr==JobReport::Done ) {
			wk = ::max( wk , snake(jr)                    .size() ) ;
			wn = ::max( wn , ::to_string(stats.ended[+jr]).size() ) ;
		}
		for( JobReport jr : All<JobReport> )
			if ( stats.ended[+jr] || jr==JobReport::Done ) {
				Color c = Color::Note ;
				switch (jr) {
					case JobReport::Failed  :
					case JobReport::LostErr : c = Color::Err     ; break ;
					case JobReport::Lost    : c = Color::Warning ; break ;
					case JobReport::Steady  :
					case JobReport::Done    : c = Color::Ok      ; break ;
				DN}
				::string t = +stats.jobs_time[+jr] ? stats.jobs_time[+jr].short_str() : ""s ;
				audit_info( c , fmt_string(::setw(wk),snake(jr)," time : ",::setw(Delay::ShortStrSz),t," (",::right,::setw(wn),stats.ended[+jr]," jobs)") ) ;
			}
		/**/                                   audit_info( Color::Note , fmt_string(::setw(wk),"elapsed"," time : " , (Pdate(New)-start_pdate)        .short_str()                  ) ) ;
		if (+options.startup_dir_s           ) audit_info( Color::Note , fmt_string(::setw(wk),"startup"," dir  : " , options.startup_dir_s.substr(0,options.startup_dir_s.size()-1)) ) ;
		//
		if (+up_to_dates) {
			static ::string src_msg       = "file is a source"       ;
			static ::string anti_msg      = "file is anti"           ;
			static ::string plain_ok_msg  = "was already up to date" ;
			static ::string plain_err_msg = "was already in error"   ;
			size_t w = 0 ;
			for( Node n : up_to_dates )
				if      (n->is_src_anti()                ) w = ::max(w,(is_target(n->name())?src_msg     :anti_msg     ).size()) ;
				else if (n->status()<=NodeStatus::Makable) w = ::max(w,(n->ok()!=No         ?plain_ok_msg:plain_err_msg).size()) ;
			for( Node n : up_to_dates )
				if      (n->is_src_anti()                ) audit_node( Color::Warning                     , fmt_string(::setw(w),is_target(n->name())?src_msg     :anti_msg     ," :") , n ) ;
				else if (n->status()<=NodeStatus::Makable) audit_node( n->ok()==No?Color::Err:Color::Note , fmt_string(::setw(w),n->ok()!=No         ?plain_ok_msg:plain_err_msg," :") , n ) ;
		}
		if (+frozen_jobs) {
			::vmap<Job,JobIdx/*order*/> frozen_jobs_ = mk_vmap(frozen_jobs) ;
			::sort( frozen_jobs_ , []( ::pair<Job,JobIdx/*order*/> const& a , ::pair<Job,JobIdx/*order*/> b ) { return a.second<b.second ; } ) ;      // sort in discovery order
			size_t w = 0 ;
			for( auto [j,_] : frozen_jobs_ ) w = ::max( w , j->rule->name.size() ) ;
			for( auto [j,_] : frozen_jobs_ ) audit_info( j->err()?Color::Err:Color::Warning , fmt_string("frozen ",::setw(w),j->rule->name) , j->name() ) ;
		}
		if (+frozen_nodes) {
			::vmap<Node,NodeIdx/*order*/> frozen_nodes_ = mk_vmap(frozen_nodes) ;
			::sort( frozen_nodes_ , []( ::pair<Node,NodeIdx/*order*/> const& a , ::pair<Node,NodeIdx/*order*/> b ) { return a.second<b.second ; } ) ; // sort in discovery order
			for( auto [n,_] : frozen_nodes_ ) audit_node( Color::Warning , "frozen " , n ) ;
		}
		if (+no_triggers) {
			::vmap<Node,NodeIdx/*order*/> no_triggers_ = mk_vmap(no_triggers) ;
			::sort( no_triggers_ , []( ::pair<Node,NodeIdx/*order*/> const& a , ::pair<Node,NodeIdx/*order*/> b ) { return a.second<b.second ; } ) ;  // sort in discovery order
			for( auto [n,_] : no_triggers_ ) audit_node( Color::Warning , "no trigger" , n ) ;
		}
		if (+clash_nodes) {
			::vmap<Node,NodeIdx/*order*/> clash_nodes_ = mk_vmap(clash_nodes) ;
			::sort( clash_nodes_ , []( ::pair<Node,NodeIdx/*order*/> const& a , ::pair<Node,NodeIdx/*order*/> b ) { return a.second<b.second ; } ) ;  // sort in discovery order
			audit_info( Color::Warning , "These files have been written by several simultaneous jobs and lmake was unable to reliably recover\n" ) ;
			for( auto [n,_] : clash_nodes_ ) audit_node(Color::Warning,{},n,1) ;
			if (job->rule->special!=Special::Req) {
				audit_info( Color::Warning , "consider : lmake -R "+mk_shell_str(job->rule->name)+" -J "+mk_shell_str(job->name()) ) ;
			} else {
				::string dl ;
				for( Dep const& d : job->deps ) dl<<' '<<mk_shell_str(d->name()) ;
				audit_info( Color::Warning , "consider : lmake"+dl ) ;
			}
		}
	}

	void ReqData::audit_job( Color c , Pdate date , ::string const& step , ::string const& rule_name , ::string const& job_name , in_addr_t host , Delay exec_time ) const {
		::OStringStream msg ;
		if (g_config->console.date_prec!=uint8_t(-1)) msg <<      date.str(g_config->console.date_prec,true/*in_day*/)     <<' '           ;
		if (g_config->console.host_len              ) msg <<      ::setw(g_config->console.host_len)<<SockFd::s_host(host) <<' '           ;
		/**/                                          msg <<      ::setw(StepSz                   )<<step                                  ;
		/**/                                          msg <<' '<< ::setw(RuleData::s_name_sz      )<<rule_name                             ;
		if (g_config->console.has_exec_time         ) msg <<' '<< ::setw(6                        )<<(+exec_time?exec_time.short_str():"") ;
		audit( audit_fd , log_stream , options , c , ::move(msg).str()+' '+mk_file(job_name) ) ;
		last_info = {} ;
	}

	static void          _audit_status( Fd out_fd, ::ostream& log , ReqOptions const& ro , bool ok )       { audit_status (out_fd  ,log       ,ro     ,ok) ; } // allow access to global function ...
	/**/   void ReqData::audit_status (                                                    bool ok ) const { _audit_status(audit_fd,log_stream,options,ok) ; } // ... w/o naming namespace

	bool/*seen*/ ReqData::audit_stderr( Job j , ::string const& msg , ::string const& stderr , size_t max_stderr_len , DepDepth lvl ) const {
		if (+msg                      ) audit_info( Color::Note , msg , lvl ) ;
		if (!stderr                   ) return +msg ;
		if (max_stderr_len!=size_t(-1)) {
			::string_view shorten = first_lines(stderr,max_stderr_len) ;
			if (shorten.size()<stderr.size()) {
				audit_info_as_is( Color::None , ::string(shorten) , lvl ) ;
				audit_info      ( Color::Note , "... (for full content : lshow -e -J "+mk_file(j->name(),FileDisplay::Shell)+" -R "+mk_shell_str(j->rule->name)+" )" , lvl ) ;
				return true ;
			}
		}
		audit_info_as_is( Color::None , stderr , lvl ) ;
		return true ;
	}

	void ReqData::audit_stats() const {
		try {
			ReqRpcReply rrr{
				ReqRpcReplyProc::Txt
			,	title(options,
					( stats.ended[+JobReport::Failed]                   ? "failed:"s   +  stats.ended[+JobReport::Failed]              +' ' : ""s )
				+	                                                      "done:"s     + (stats.done()-stats.ended[+JobReport::Failed])
				+	( +g_config->caches && stats.ended[+JobReport::Hit] ? " hit:"s     +  stats.ended[+JobReport::Hit   ]                   : ""s )
				+	( stats.ended[+JobReport::Rerun ]                   ? " rerun:"s   +  stats.ended[+JobReport::Rerun ]                   : ""s )
				+	                                                      " running:"s +  stats.cur(JobStep::Exec  )
				+	( stats.cur(JobStep::Queued)                        ? " queued:"s  +  stats.cur(JobStep::Queued)                        : ""s )
				+	( stats.cur(JobStep::Dep   )>1                      ? " waiting:"s + (stats.cur(JobStep::Dep   )-1                )     : ""s ) // suppress job representing Req itself
				+	( g_config->console.show_eta                        ? " - ETA:"s   +  eta.str(0/*prec*/,true/*in_day*/)                 : ""s )
				)
			} ;
			OMsgBuf().send( audit_fd , rrr ) ;
		} catch (::string const&) {}           // if client has disappeared, well, we cannot do much
	}

	bool/*overflow*/ ReqData::_send_err( bool intermediate , ::string const& pfx , ::string const& target , size_t& n_err , DepDepth lvl ) {
		if (!n_err) return true/*overflow*/ ;
		n_err-- ;
		if (n_err) audit_info( intermediate?Color::HiddenNote:Color::Err , fmt_string(::setw(::max(size_t(8)/*dangling*/,RuleData::s_name_sz)),pfx) , target , lvl ) ;
		else       audit_info( Color::Warning                            , "..."                                                                                   ) ;
		return !n_err/*overflow*/ ;
	}

	void ReqData::_report_no_rule( Node node , NfsGuard& nfs_guard , DepDepth lvl ) {
		::string                          name      = node->name() ;
		::vmap<RuleTgt,Rule::SimpleMatch> mrts      ;                                                   // matching rules
		RuleTgt                           art       ;                                                   // set if an anti-rule matches
		RuleIdx                           n_missing = 0            ;                                    // number of rules missing deps
		//
		if (name.size()>g_config->path_max) {
			audit_node( Color::Warning , "name is too long :" , node , lvl ) ;
			return ;
		}
		//
		if ( node->status()==NodeStatus::Uphill || node->status()==NodeStatus::Transient ) {
			Node dir ; for( dir=node->dir() ; +dir && (dir->status()==NodeStatus::Uphill||dir->status()==NodeStatus::Transient) ; dir=dir->dir() ) ;
			swear_prod(+dir                              ,"dir is buildable for",name,"but cannot find buildable dir"                  ) ;
			swear_prod(dir->status()<=NodeStatus::Makable,"dir is buildable for",name,"but cannot find buildable dir until",dir->name()) ;
			/**/                                audit_node( Color::Err  , "no rule for"        , node , lvl   ) ;
			if (dir->status()==NodeStatus::Src) audit_node( Color::Note , "dir is a source :"  , dir  , lvl+1 ) ;
			else                                audit_node( Color::Note , "dir is buildable :" , dir  , lvl+1 ) ;
			return ;
		}
		//
		for( RuleTgt rt : Node::s_rule_tgts(name).view() ) {                                            // first pass to gather info : mrts : matching rules, n_missing : number of missing deps
			Rule::SimpleMatch m{rt,name} ;
			if (!m                        )              continue ;
			if (rt->special==Special::Anti) { art = rt ; break    ; }
			//
			if ( JobTgt jt{rt,name} ; +jt && jt->run_status!=RunStatus::MissingStatic ) goto Continue ; // do not pass *this as req to avoid generating error message at cxtor time
			try                      { rt->deps_attrs.eval(m) ; }
			catch (::pair_ss const&) { goto Continue ;          }                                       // do not consider rule if deps cannot be computed
			n_missing++ ;
		Continue :
			mrts.emplace_back(rt,::move(m)) ;
		}
		//
		if ( !art && !mrts                             ) audit_node( Color::Err  , "no rule match"      , node , lvl   ) ;
		else                                             audit_node( Color::Err  , "no rule for"        , node , lvl   ) ;
		if ( !art && is_target(nfs_guard.access(name)) ) audit_node( Color::Note , "consider : git add" , node , lvl+1 ) ;
		//
		for( auto const& [rt,m] : mrts ) {                                                              // second pass to do report
			JobTgt            jt          { rt , name } ;                                               // do not pass *this as req to avoid generating error message at cxtor time
			::string          reason      ;
			Node              missing_dep ;
			::vmap_s<DepSpec> static_deps ;
			if ( +jt && jt->run_status!=RunStatus::MissingStatic ) { reason      = "does not produce it"                                      ; goto Report ; }
			try                                                    { static_deps = rt->deps_attrs.eval(m)                                     ;               }
			catch (::pair_ss const& msg_err)                       { reason      = "cannot compute its deps :\n"+msg_err.first+msg_err.second ; goto Report ; }
			{	::string missing_key ;
				for( bool search_non_buildable : {true,false} )                                         // first search a non-buildable, if not found, search for non makable as deps have been made
					for( auto const& [k,dn] : static_deps ) {
						Node d{dn.txt} ;
						if ( search_non_buildable ? d->buildable>Buildable::No : d->status()<=NodeStatus::Makable ) continue ;
						missing_key = k ;
						missing_dep = d ;
						goto Found ;
					}
			Found :
				SWEAR(+missing_dep) ;                                                                   // else why wouldn't it apply ?!?
				::string mdn = missing_dep->name()                   ;
				FileTag tag  = FileInfo(nfs_guard.access(mdn)).tag() ;
				reason = "misses static dep " + missing_key + (tag>=FileTag::Target?" (existing)":tag==FileTag::Dir?" (dir)":"") ;
			}
		Report :
			if (+missing_dep) audit_node( Color::Note , "rule "+rt->name+' '+reason+" :" , missing_dep , lvl+1 ) ;
			else              audit_info( Color::Note , "rule "+rt->name+' '+reason      ,               lvl+1 ) ;
			//
			if ( +missing_dep && n_missing==1 && (!g_config->max_err_lines||lvl<g_config->max_err_lines) ) _report_no_rule( missing_dep , nfs_guard , lvl+2 ) ;
		}
		//
		if (+art) audit_info( Color::Note , "anti-rule "+art->name+" matches" , lvl+1 ) ;
	}

	//
	// JobAudit
	//

	::ostream& operator<<( ::ostream& os , JobAudit const& ja ) {
		/**/                 os << "JobAudit(" << ja.report ;
		if (+ja.backend_msg) os <<','<< ja.backend_msg      ;
		return               os <<')'                       ;

	}

}
