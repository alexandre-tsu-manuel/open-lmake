// This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#include "core.hh"

#include <tuple>

#include "rpc_job.hh"

using Backends::Backend ;

using namespace Disk ;

namespace Engine {

	SeqId     * g_seq_id     = nullptr ;
	Config    * g_config     = nullptr ;
	::vector_s* g_src_dirs_s = nullptr ;

}

namespace Engine::Persistent {

	//
	// RuleBase
	//

	MatchGen         RuleBase::s_match_gen = 1 ; // 0 is forbidden as it is reserved to mean !match
	umap_s<RuleBase> RuleBase::s_by_name   ;

	//
	// NodeBase
	//

	RuleTgts NodeBase::s_rule_tgts(::string const& target_name) {
		// first match on suffix
		PsfxIdx sfx_idx = _sfxs_file.longest(target_name,::string{Persistent::StartMrkr}).first ; // StartMrkr is to match rules w/ no stems
		if (!sfx_idx) return RuleTgts{} ;
		PsfxIdx pfx_root = _sfxs_file.c_at(sfx_idx) ;
		// then match on prefix
		PsfxIdx pfx_idx = _pfxs_file.longest(pfx_root,target_name).first ;
		if (!pfx_idx) return RuleTgts{} ;
		return _pfxs_file.c_at(pfx_idx) ;

	}

	//
	// Persistent
	//

	bool writable = false ;

	// on disk
	JobFile      _job_file       ; // jobs
	DepsFile     _deps_file      ; // .
	TargetsFile  _targets_file   ; // .
	NodeFile     _node_file      ; // nodes
	JobTgtsFile  _job_tgts_file  ; // .
	RuleStrFile  _rule_str_file  ; // rules
	RuleFile     _rule_file      ; // .
	RuleTgtsFile _rule_tgts_file ; // .
	SfxFile      _sfxs_file      ; // .
	PfxFile      _pfxs_file      ; // .
	NameFile     _name_file      ; // commons
	// in memory
	::uset<Job >       _frozen_jobs  ;
	::uset<Node>       _frozen_nodes ;
	::uset<Node>       _no_triggers  ;
	::vector<RuleData> _rule_datas   ;

	static void _compile_srcs() {
		if (g_src_dirs_s) g_src_dirs_s->clear() ;
		else              g_src_dirs_s = new ::vector_s ;
		for( Node const n : Node::s_srcs(true/*dirs*/) ) {
			::string nn_s = n->name() ; nn_s += '/' ;
			g_src_dirs_s->push_back(nn_s) ;
		}
	}

	static void _compile_rule_datas() {
		::vector<Rule> rules = rule_lst() ;
		RuleData::s_name_sz = "no_rule"s.size() ;                         // account for internal names
		_rule_datas.clear() ;                                             // clearing before resize ensure all unused entries are clean
		for( Special s : All<Special> ) if ( +s && s<=Special::Shared ) {
			grow(_rule_datas,+s) = RuleData(s)                                               ;
			RuleData::s_name_sz = ::max( RuleData::s_name_sz , _rule_datas[+s].name.size() ) ;
		}
		for( Rule r : rules ) {
			grow(_rule_datas,+r) = r.str()                                                   ;
			RuleData::s_name_sz = ::max( RuleData::s_name_sz , _rule_datas[+r].name.size() ) ;
		}
	}

	static void _compile_rules() {
		_compile_rule_datas() ;
		RuleBase::s_by_name.clear() ;
		for( Rule r : rule_lst() ) RuleBase::s_by_name[r->name] = r ;
	}

	static void _init_config() {
		try         { g_config = new Config{deserialize<Config>(IFStream(PrivateAdminDirS+"config_store"s))} ; }
		catch (...) { g_config = new Config                                                                  ; }
	}

	static void _init_srcs_rules(bool rescue) {
		Trace trace("_init_srcs_rules",Pdate(New)) ;
		//
		// START_OF_VERSIONING
		::string dir_s = g_config->local_admin_dir_s+"store/" ;
		//
		mk_dir_s(dir_s) ;
		// jobs
		_job_file      .init( dir_s+"job"       , writable ) ;
		_deps_file     .init( dir_s+"deps"      , writable ) ;
		_targets_file  .init( dir_s+"_targets"  , writable ) ;
		// nodes
		_node_file     .init( dir_s+"node"      , writable ) ;
		_job_tgts_file .init( dir_s+"job_tgts"  , writable ) ;
		// rules
		_rule_str_file .init( dir_s+"rule_str"  , writable ) ;
		_rule_file     .init( dir_s+"rule"      , writable ) ; if ( writable && !_rule_file.c_hdr() ) _rule_file.hdr() = 1 ; // 0 is reserved to mean no match
		_rule_tgts_file.init( dir_s+"rule_tgts" , writable ) ;
		_sfxs_file     .init( dir_s+"sfxs"      , writable ) ;
		_pfxs_file     .init( dir_s+"pfxs"      , writable ) ;
		// commons
		_name_file     .init( dir_s+"name"      , writable ) ;
		// misc
		if (writable) {
			g_seq_id = &_job_file.hdr().seq_id ;
			if (!*g_seq_id) *g_seq_id = 1 ;                                                                                  // avoid 0 (when store is brand new) to decrease possible confusion
		}
		// Rule
		if (!_rule_file) for( [[maybe_unused]] Special s : Special::Shared ) _rule_file.emplace() ;
		RuleBase::s_match_gen = _rule_file.c_hdr() ;
		// END_OF_VERSIONING
		//
		SWEAR(RuleBase::s_match_gen>0) ;
		_job_file      .keep_open = true ; // files may be needed post destruction as there may be alive threads as we do not masterize destruction order
		_deps_file     .keep_open = true ; // .
		_targets_file  .keep_open = true ; // .
		_node_file     .keep_open = true ; // .
		_job_tgts_file .keep_open = true ; // .
		_rule_str_file .keep_open = true ; // .
		_rule_file     .keep_open = true ; // .
		_rule_tgts_file.keep_open = true ; // .
		_sfxs_file     .keep_open = true ; // .
		_pfxs_file     .keep_open = true ; // .
		_name_file     .keep_open = true ; // .
		_compile_srcs () ;
		_compile_rules() ;
		for( Job  j : _job_file .c_hdr().frozens    ) _frozen_jobs .insert(j) ;
		for( Node n : _node_file.c_hdr().frozens    ) _frozen_nodes.insert(n) ;
		for( Node n : _node_file.c_hdr().no_triggers) _no_triggers .insert(n) ;
		//
		trace("done",Pdate(New)) ;
		//
		if (rescue) {
			::cerr<<"previous crash detected, checking & rescueing"<<endl ;
			try {
				chk()              ;       // first verify we have a coherent store
				invalidate_match() ;       // then rely only on essential data that should be crash-safe
				::cerr<<"seems ok"<<endl ;
			} catch (::string const&) {
				exit(Rc::Format,"failed to rescue, consider running lrepair") ;
			}
		}
	}


	void chk() {
		// files
		/**/                                  _job_file      .chk(                    ) ; // jobs
		/**/                                  _deps_file     .chk(                    ) ; // .
		/**/                                  _targets_file  .chk(                    ) ; // .
		/**/                                  _node_file     .chk(                    ) ; // nodes
		/**/                                  _job_tgts_file .chk(                    ) ; // .
		/**/                                  _rule_str_file .chk(                    ) ; // rules
		/**/                                  _rule_file     .chk(                    ) ; // .
		/**/                                  _rule_tgts_file.chk(                    ) ; // .
		/**/                                  _sfxs_file     .chk(                    ) ; // .
		for( PsfxIdx idx : _sfxs_file.lst() ) _pfxs_file     .chk(_sfxs_file.c_at(idx)) ; // .
		/**/                                  _name_file     .chk(                    ) ; // commons
	}

	static void _save_config() {
		serialize( OFStream(PrivateAdminDirS+"config_store"s) , *g_config ) ;
		OFStream(AdminDirS+"config"s) << g_config->pretty_str() ;
	}

	static void _diff_config( Config const& old_config , bool dynamic ) {
		Trace trace("_diff_config",old_config) ;
		for( BackendTag t : All<BackendTag> ) {
			if (g_config->backends[+t].ifce==old_config.backends[+t].ifce) continue ;
			if (dynamic                                                  ) throw "cannot change server address while running"s ; // remote hosts may have been unreachable ...
			invalidate_exec(true/*cmd_ok*/) ;                                                                                    // ... do as if we have new resources
			break ;
		}
		//
		if (g_config->path_max!=old_config.path_max) invalidate_match() ;                                                        // we may discover new buildable nodes or vice versa
	}

	void new_config( Config&& config , bool dynamic , bool rescue , ::function<void(Config const& old,Config const& new_)> diff ) {
		Trace trace("new_config",Pdate(New),STR(dynamic),STR(rescue)) ;
		if ( !dynamic                                               ) mk_dir_s( AdminDirS+"outputs/"s , true/*unlnk_ok*/ ) ;
		if ( !dynamic                                               ) _init_config() ;
		else                                                          SWEAR(g_config->booted,*g_config) ; // we must update something
		if (                                       g_config->booted ) config.key = g_config->key ;
		//
		/**/                                                         diff(*g_config,config) ;
		//
		/**/                                                         ConfigDiff d = config.booted ? g_config->diff(config) : ConfigDiff::None ;
		if (              d>ConfigDiff::Static  && g_config->booted ) throw "repo must be clean"s  ;
		if (  dynamic &&  d>ConfigDiff::Dynamic                     ) throw "repo must be steady"s ;
		//
		if (  dynamic && !d                                         ) return ;                            // fast path, nothing to update
		//
		/**/                                                          Config old_config = *g_config ;
		if (             +d                                         ) *g_config = ::move(config) ;
		if (!g_config->booted) throw "no config available"s ;
		/**/                                                          g_config->open(dynamic)          ;
		if (             +d                                         ) _save_config()                   ;
		if ( !dynamic                                               ) _init_srcs_rules(rescue)         ;
		if (             +d                                         ) _diff_config(old_config,dynamic) ;
		trace("done",Pdate(New)) ;
	}

	void repair(::string const& from_dir_s) {
		Trace trace("repair",from_dir_s) ;
		::vector<Rule>   rules    = rule_lst() ;
		::umap<Crc,Rule> rule_tab ; for( Rule r : Rule::s_lst() ) rule_tab[r->cmd_crc] = r ; SWEAR(rule_tab.size()==rules.size()) ;
		for( ::string const& jd : walk(no_slash(from_dir_s),no_slash(from_dir_s)) ) {
			{	JobInfo job_info { jd } ;
				// qualify report
				if (job_info.start.pre_start.proc!=JobRpcProc::Start) { trace("no_pre_start",jd) ; goto NextJob ; }
				if (job_info.start.start    .proc!=JobRpcProc::Start) { trace("no_start"    ,jd) ; goto NextJob ; }
				if (job_info.end  .end      .proc!=JobRpcProc::End  ) { trace("no_end"      ,jd) ; goto NextJob ; }
				if (job_info.end  .end.digest.status!=Status::Ok    ) { trace("not_ok"      ,jd) ; goto NextJob ; }         // repairing jobs in error is useless
				// find rule
				auto it = rule_tab.find(job_info.start.rule_cmd_crc) ;
				if (it==rule_tab.end()) { trace("no_rule",jd) ; goto NextJob ; }                                            // no rule
				Rule rule = it->second ;
				// find targets
				::vector<Target> targets ; targets.reserve(job_info.end.end.digest.targets.size()) ;
				for( auto const& [tn,td] : job_info.end.end.digest.targets ) {
					if ( td.crc==Crc::None && !static_phony(td.tflags) ) continue     ;                                     // this is not a target
					if ( !td.crc.valid()                               ) { trace("invalid_target",jd,tn) ; goto NextJob ; } // XXX : handle this case
					if ( td.sig!=FileSig(tn)                           ) { trace("disk_mismatch" ,jd,tn) ; goto NextJob ; } // if dates do not match, we will rerun the job anyway
					//
					Node t{tn} ;
					t->refresh( td.crc , {td.sig,{}} ) ;                                                                    // if file does not exist, the Epoch as a date is fine
					targets.emplace_back( t , td.tflags ) ;
				}
				::sort(targets) ;                                                                              // ease search in targets
				// find deps
				::vector_s    src_dirs ; for( Node s : Node::s_srcs(true/*dirs*/) ) src_dirs.push_back(s->name()) ;
				::vector<Dep> deps     ; deps.reserve(job_info.end.end.digest.deps.size()) ;
				for( auto const& [dn,dd] : job_info.end.end.digest.deps ) {
					if ( !is_canon(dn)) goto NextJob ;                                                         // this should never happen, there is a problem with this job
					if (!is_lcl(dn)) {
						for( ::string const& sd : src_dirs ) if (dn.starts_with(sd)) goto KeepDep ;            // this could be optimized by searching the longest match in the name prefix tree
						goto NextJob ;                                                                         // this should never happen as src_dirs are part of cmd definition
					KeepDep : ;
					}
					Dep dep { Node(dn) , dd } ;
					if ( !dep.is_crc                         ) { trace("no_dep_crc" ,jd,dn) ; goto NextJob ; } // dep could not be identified when job ran, hum, better not to repair that
					if ( +dep.accesses && !dep.crc().valid() ) { trace("invalid_dep",jd,dn) ; goto NextJob ; } // no valid crc, no interest to repair as job will rerun anyway
					deps.emplace_back(dep) ;
				}
				// set job
				Job job { {rule,::move(job_info.start.stems)} } ;
				if (!job) goto NextJob ;
				job->targets.assign(targets) ;
				job->deps   .assign(deps   ) ;
				job->status = job_info.end.end.digest.status ;
				job->exec_ok(true) ;                                                                           // pretend job just ran
				// set target actual_job's
				for( Target t : targets ) {
					t->actual_job   () = job      ;
					t->actual_tflags() = t.tflags ;
				}
				// restore job_data
				job.record(job_info) ;
				trace("restored",jd,job->name()) ;
			}
		NextJob : ;
		}
	}

	// str has target syntax
	// return suffix after last stem (StartMrkr+str if no stem)
	static ::string _parse_sfx(::string const& str) {
		size_t pos = 0 ;
		for(;;) {
			size_t nxt_pos = str.find(Rule::StemMrkr,pos) ;
			if (nxt_pos==Npos) break ;
			pos = nxt_pos+1+sizeof(VarIdx) ;
		}
		if (pos==0) return StartMrkr+str   ; // signal that there is no stem by prefixing with StartMrkr
		else        return str.substr(pos) ; // suppress stem marker & stem idx
	}

	// return prefix before first stem (empty if no stem)
	static ::string _parse_pfx(::string const& str) {
		size_t pos = str.find(Rule::StemMrkr) ;
		if (pos==Npos) return {}                ; // absence of stem is already signal in _parse_sfx, we just need to pretend there is no prefix
		else           return str.substr(0,pos) ;
	}

	struct Rt : RuleTgt {
		// cxtors & casts
		Rt() = default  ;
		Rt( Rule r , VarIdx ti ) : RuleTgt{r,ti} , pfx{_parse_pfx(target())} , sfx{_parse_sfx(target())} {}
		// data (cache)
		::string pfx ;
		::string sfx ;
	} ;

}

namespace std {
	template<> struct hash<Engine::Persistent::Rt> {
		size_t operator()(Engine::Persistent::Rt const& rt) const { return hash<Engine::RuleTgt>()(rt) ; } // there is no more info in a Rt than in a RuleTgt
	} ;
}

namespace Engine::Persistent {

	static void _invalidate_exec(::vector<pair<bool,ExecGen>> const& keep_cmd_gens) {
		for( auto [yes,cmd_gen] : keep_cmd_gens ) if (yes) goto FullPass ;
		return ;
	FullPass :
		Trace trace("_invalidate_exec") ;
		Trace trace2 ;
		::cerr << "collecting job cmds ..." ;
		for( Job j : job_lst() ) {
			if (j->rule.is_shared()) continue ;
			auto [yes,cmd_gen] = keep_cmd_gens[+j->rule] ;
			if (!yes) continue ;
			ExecGen old_exec_gen = j->exec_gen ;
			j->exec_gen = cmd_gen && j->exec_gen >= cmd_gen ; // set to 0 if bad cmd, to 1 if cmd ok but bad rsrcs
			trace2(j,j->rule,old_exec_gen,"->",j->exec_gen) ;
		}
		::cerr << endl ;
	}

	template<bool IsSfx> static void _propag_to_longer(::umap_s<uset<Rt>>& psfx_map) {
		::vector_s psfxs = ::mk_key_vector(psfx_map) ;
		::sort( psfxs , [](::string const& a,::string const& b){ return a.size()<b.size() ; } ) ;
		for( ::string const& long_psfx : psfxs ) {
			for( size_t l=1 ; l<=long_psfx.size() ; l++ ) {
				::string short_psfx = long_psfx.substr( IsSfx?l:0 , long_psfx.size()-l ) ;
				if (psfx_map.contains(short_psfx)) {
					psfx_map.at(long_psfx).merge(::copy(psfx_map.at(short_psfx))) ; // copy arg as merge clobbers it
					break ;                                                         // psfx's are sorted shortest first, so as soon as a short one is found, it is already merged with previous ones
				}
			}
		}
	}

	// make a prefix/suffix map that records which rule has which prefix/suffix
	static void _compile_psfxs() {
		_sfxs_file.clear() ;
		_pfxs_file.clear() ;
		// first compute a suffix map
		::umap_s<uset<Rt>> sfx_map ;
		for( Rule r : rule_lst() )
			for( VarIdx ti=0 ; ti<r->matches.size() ; ti++ ) {
				if ( r->matches[ti].second.flags.is_target!=Yes         ) continue ;
				if (!r->matches[ti].second.flags.tflags()[Tflag::Target]) continue ;
				Rt rt{r,ti} ;
				sfx_map[rt.sfx].insert(rt) ;
			}
		_propag_to_longer<true/*IsSfx*/>(sfx_map) ;          // propagate to longer suffixes as a rule that matches a suffix also matches any longer suffix
		//
		// now, for each suffix, compute a prefix map
		for( auto const& [sfx,sfx_rule_tgts] : sfx_map ) {
			::umap_s<uset<Rt>> pfx_map ;
			if ( sfx.starts_with(StartMrkr) ) {
				::string_view sfx1 = ::string_view(sfx).substr(1) ;
				for( Rt const& rt : sfx_rule_tgts )
					if (sfx1.starts_with(rt.pfx)) pfx_map[""].insert(rt) ;
			} else {
				for( Rt const& rt : sfx_rule_tgts )
					pfx_map[rt.pfx].insert(rt) ;
				_propag_to_longer<false/*IsSfx*/>(pfx_map) ; // propagate to longer prefixes as a rule that matches a prefix also matches any longer prefix
			}
			//
			// store proper rule_tgts (ordered by decreasing prio, giving priority to AntiRule within each prio) for each prefix/suffix
			PsfxIdx pfx_root = _pfxs_file.emplace_root() ;
			_sfxs_file.insert_at(sfx) = pfx_root ;
			for( auto const& [pfx,pfx_rule_tgts] : pfx_map ) {
				vector<Rt> pfx_rule_tgt_vec = mk_vector(pfx_rule_tgts) ;
				::sort(
					pfx_rule_tgt_vec
				,	[]( Rt const& a , Rt const& b )->bool {
						// compulsery : order by priority, with special Rule's before plain Rule's, with Anti's before GenericSrc's within each priority level
						// optim      : put more specific rules before more generic ones to favor sharing RuleTgts in reversed PrefixFile
						// finally    : any stable sort is fine, just to avoid random order
						return
							::tuple( a->is_special() , a->prio , a->special , a.pfx.size()+a.sfx.size() , a->name )
						>	::tuple( b->is_special() , b->prio , b->special , b.pfx.size()+b.sfx.size() , b->name )
						;
					}
				) ;
				_pfxs_file.insert_at(pfx_root,pfx) = RuleTgts(mk_vector<RuleTgt>(pfx_rule_tgt_vec)) ;
			}
		}
	}

	static void _save_rules() {
		_rule_str_file.clear() ;
		for( Rule r : rule_lst() ) _rule_file.at(r) = _rule_str_file.emplace(serialize(_rule_datas[+r])) ;
	}

	static void _set_exec_gen( RuleData& rd , ::pair<bool,ExecGen>& keep_cmd_gen , bool cmd_ok , bool dynamic=false ) { // called if at least resources changed
		Trace trace("_set_exec_gen") ;
		if (rd.rsrcs_gen<NExecGen-1) {
			if (!cmd_ok) rd.cmd_gen   = rd.rsrcs_gen+1 ;
			/**/         rd.rsrcs_gen = rd.rsrcs_gen+1 ;
			Trace trace("up gen",rd.cmd_gen,rd.rsrcs_gen) ;
		} else {
			if (cmd_ok) keep_cmd_gen = {true,rd.cmd_gen} ;    // all cmd_gen above this will be set to 1 to keep cmd, the others to 0
			else        keep_cmd_gen = {true,0         } ;    // all cmd_gen must be set to 0 as we have a new cmd but no room left for a new cmd_gen
			rd.cmd_gen   = 1                 ;                // 0 is reserved to force !cmd_ok
			rd.rsrcs_gen = rd.cmd_gen+cmd_ok ;                // if cmd_ok, we must distinguish between bad cmd and good cmd with bad rsrcs
			trace("reset gen",rd.cmd_gen,rd.rsrcs_gen) ;
		}
		if (dynamic) SWEAR( !keep_cmd_gen.first && cmd_ok ) ; // ensure decision of making job is not pertubated
	}

	void invalidate_exec(bool cmd_ok) {
		Trace trace("invalidate_exec",STR(cmd_ok)) ;
		::vector<pair<bool,ExecGen>> keep_cmd_gens{_rule_datas.size()} ; // indexed by Rule, if entry.first => entry.second is 0 (always reset exec_gen) or exec_gen w/ cmd_ok but !rsrcs_ok
		for( Rule r : rule_lst() ) {
			_set_exec_gen( _rule_datas[+r] , keep_cmd_gens[+r] , cmd_ok ) ;
			trace(r,r->cmd_gen,r->rsrcs_gen) ;
		}
		_save_rules() ;
		_invalidate_exec(keep_cmd_gens) ;
	}

	static void _collect_old_rules() {                                                               // may be long, avoid as long as possible
		MatchGen& match_gen = _rule_file.hdr() ;
		Trace("_collect_old_rules","reset",1) ;
		::cerr << "collecting" ;
		::cerr << " nodes ..." ; for( Node     n   : node_lst           () ) n  ->mk_old()         ; // handle nodes first as jobs are necessary at this step
		::cerr << " jobs ..."  ; for( Job      j   : job_lst            () ) j  ->invalidate_old() ;
		::cerr << " rules ..." ; for( RuleTgts rts : _rule_tgts_file.lst() ) rts. invalidate_old() ;
		/**/                     for( Rule     r   : rule_lst           () ) r  . invalidate_old() ; // now that old rules are not referenced any more, they can be suppressed and reused
		::cerr << endl ;
		Rule::s_match_gen = match_gen = 1 ;
	}

	bool/*invalidate*/ new_rules( ::umap<Crc,RuleData>&& new_rules_ , bool dynamic ) {
		Trace trace("new_rules",new_rules_.size()) ;
		//
		RuleIdx           max_old_rule = 0 ;
		::umap <Crc,Rule> old_rules    ;
		for( Rule r : rule_lst() ) {
			max_old_rule            = ::max(max_old_rule,+r) ;
			old_rules[r->match_crc] = r                      ;
		}
		//
		RuleIdx n_new_rules       = 0     ;
		RuleIdx n_modified_cmd    = 0     ;
		RuleIdx n_modified_rsrcs  = 0     ;
		RuleIdx n_modified_prio   = 0     ;
		bool    missing_rsrcs_gen = false ;
		// evaluate diff
		for( auto& [match_crc,new_rd] : new_rules_ ) {
			auto it = old_rules.find(match_crc) ;
			if (it==old_rules.end()) {
				n_new_rules++ ;
			} else {
				Rule old_r          = it->second                         ;
				bool modified_rsrcs = new_rd.rsrcs_crc!=old_r->rsrcs_crc ;
				n_modified_prio   += new_rd.prio   !=old_r->prio                      ;
				n_modified_cmd    += new_rd.cmd_crc!=old_r->cmd_crc                   ;
				n_modified_rsrcs  += modified_rsrcs                                   ;
				missing_rsrcs_gen |= modified_rsrcs && old_r->rsrcs_gen>=(NExecGen-1) ;
			}
		}
		if (dynamic) {                                                                        // check if compatible with dynamic update
			if (n_new_rules                        ) throw "new rules"s                     ;
			if (old_rules.size()!=new_rules_.size()) throw "old rules"s                     ;
			if (n_modified_prio                    ) throw "rule priorities were modified"s ;
			if (n_modified_cmd                     ) throw "rule cmd's were modified"s      ;
			if (missing_rsrcs_gen                  ) throw "must garbage collect rules"s    ;
		}
		//
		for( auto const& [crc,r] : old_rules )                        // make old rules obsolete but do not pop to prevent idx reuse as long as old rules are not collected
			if (!new_rules_.contains(crc)) _rule_file.clear(r) ;
		if ( old_rules.size()+n_new_rules >= RuleIdx(-1) ) {          // in case of size overflow, physically collect obsolete rules as we cannot fit old & new rules
			SWEAR(!dynamic) ;                                         // cannot happen without new rules
			_collect_old_rules() ;
		}
		::vector<pair<bool,ExecGen>> keep_cmd_gens{max_old_rule+1u} ; // indexed by Rule, if entry.first => entry.second is 0 (always reset exec_gen) or exec_gen w/ cmd_ok but !rsrcs_ok
		//
		_rule_str_file.clear() ;                                      // erase old rules before recording new ones
		for( auto& [match_crc,new_rd] : new_rules_ ) {
			Rule old_r ;
			auto it = old_rules.find(match_crc) ;
			if (it==old_rules.end()) {
				trace("new",new_rd,new_rd.cmd_gen,new_rd.rsrcs_gen) ;
			} else {
				old_r = it->second ;
				old_rules.erase(it) ;
				new_rd.cmd_gen        = old_r->cmd_gen        ;
				new_rd.rsrcs_gen      = old_r->rsrcs_gen      ;
				new_rd.cost_per_token = old_r->cost_per_token ;
				new_rd.exec_time      = old_r->exec_time      ;
				new_rd.stats_weight   = old_r->stats_weight   ;
				SWEAR( +old_r<keep_cmd_gens.size() , old_r , keep_cmd_gens.size() ) ;
				if (new_rd.rsrcs_crc!=old_r->rsrcs_crc) {
					bool cmd_ok = new_rd.cmd_crc==old_r->cmd_crc ;
					_set_exec_gen( new_rd , keep_cmd_gens[+old_r] , cmd_ok , dynamic ) ;
					trace("modified",new_rd,STR(cmd_ok),new_rd.cmd_gen,new_rd.rsrcs_gen) ;
				}
			}
			//           vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
			RuleStr rs = _rule_str_file.emplace(serialize(new_rd)) ;
			//           ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
			if (+old_r) _rule_file.at(old_r) = rs ;
			else        _rule_file.emplace(rs) ;
		}
		bool res = n_modified_prio || n_new_rules || +old_rules ;
		trace("rules",'-',old_rules.size(),'+',n_new_rules,"=cmd",n_modified_cmd,"=rsrcs",n_modified_rsrcs,"=prio",n_modified_prio) ;
		//
		_compile_rules() ;                                            // recompute derived info in memory
		if (res) _compile_psfxs() ;                                   // recompute derived info on disk
		_invalidate_exec(keep_cmd_gens) ;
		// trace
		Trace trace2 ;
		for( PsfxIdx sfx_idx : _sfxs_file.lst() ) {
			::string sfx      = _sfxs_file.str_key(sfx_idx) ;
			PsfxIdx  pfx_root = _sfxs_file.at     (sfx_idx) ;
			bool     single   = +sfx && sfx[0]==StartMrkr  ;
			for( PsfxIdx pfx_idx : _pfxs_file.lst(pfx_root) ) {
				RuleTgts rts = _pfxs_file.at(pfx_idx) ;
				::string pfx = _pfxs_file.str_key(pfx_idx) ;
				if (single) { SWEAR(!pfx,pfx) ; trace2(         sfx.substr(1) , ':' ) ; }
				else        {                   trace2( pfx+'*'+sfx           , ':' ) ; }
				Trace trace3 ;
				for( RuleTgt rt : rts.view() ) trace3( +Rule(rt) , ':' , rt->prio , rt->name , rt.key() ) ;
			}
		}
		// user report
		{	OFStream rules_stream{AdminDirS+"rules"s} ;
			::vector<Rule> rules = rule_lst() ;
			::sort( rules , [](Rule a,Rule b){
				if (a->prio!=b->prio) return a->prio > b->prio ;
				else                  return a->name < b->name ;
			} ) ;
			bool first = true ;
			for( Rule rule : rules ) {
				if (!rule->user_defined()) continue ;
				if (first) first = false ;
				else       rules_stream << '\n' ;
				rules_stream<<rule->pretty_str() ;
			}
		}
		return res ;
	}

	bool/*invalidate*/ new_srcs( ::pair<::vmap_s<FileTag>/*files*/,::vector_s/*dirs_s*/>&& src_names , bool dynamic ) {
		::map<Node,FileTag    > srcs         ;                                                                                                  // use ordered map/set to ensure stable execution
		::map<Node,FileTag    > old_srcs     ;                                                                                                  // .
		::map<Node,FileTag    > new_srcs_    ;                                                                                                  // .
		::set<Node            > src_dirs     ;                                                                                                  // .
		::set<Node            > old_src_dirs ;                                                                                                  // .
		::set<Node            > new_src_dirs ;                                                                                                  // .
		Trace trace("new_srcs") ;
		//
		size_t          root_dir_depth      = 0       ; { for( char c : *g_root_dir_s ) root_dir_depth += c=='/' ; } root_dir_depth-- ;         // there is one more / than the actual depth
		size_t          src_dirs_uphill_lvl = 0       ;
		::string const* highest             = nullptr ;
		for( ::string const& d_s : src_names.second ) {
			if (!is_abs_s(d_s))
				if ( size_t ul=uphill_lvl_s(d_s) ; ul>src_dirs_uphill_lvl ) {
					src_dirs_uphill_lvl = ul   ;
					highest             = &d_s ;
				}
		}
		if (root_dir_depth<=src_dirs_uphill_lvl) {
			SWEAR(highest) ;
			throw "cannot access relative source dir "+no_slash(*highest)+" from repository "+no_slash(*g_root_dir_s) ;
		}
		// format inputs
		for( bool dirs : {false,true} ) for( Node s : Node::s_srcs(dirs) ) old_srcs.emplace(s,dirs?FileTag::Dir:FileTag::None) ;                // dont care whether we delete a regular file or a link
		//
		for( auto const& [sn,t] : src_names.first  )                   srcs.emplace( Node(sn                      ) , t                   ) ;
		for( ::string&    sn    : src_names.second ) { sn.pop_back() ; srcs.emplace( Node(sn,!is_lcl(sn)/*no_dir*/) , FileTag::Dir/*dir*/ ) ; } // external src dirs need no uphill dir
		//
		for( auto [n,_] : srcs     ) for( Node d=n->dir() ; +d ; d = d->dir() ) if (!src_dirs    .insert(d).second) break ;                     // non-local nodes have no dir
		for( auto [n,_] : old_srcs ) for( Node d=n->dir() ; +d ; d = d->dir() ) if (!old_src_dirs.insert(d).second) break ;                     // .
		// check
		for( auto [n,t] : srcs ) {
			if (!src_dirs.contains(n)) continue ;
			::string nn   = n->name() ;
			::string nn_s = nn+'/'    ;
			for( auto const& [sn,_] : src_names.first )
				if ( sn.starts_with(nn_s) ) throw "source "s+(t==FileTag::Dir?"dir ":"")+nn+" is a dir of "+sn ;
			FAIL(nn,"is a source dir of no source") ;
		}
		// compute diff
		bool fresh = !old_srcs ;
		for( auto nt : srcs ) {
			auto it = old_srcs.find(nt.first) ;
			if (it==old_srcs.end()) new_srcs_.insert(nt) ;
			else                    old_srcs .erase (it) ;
		}
		if (!fresh) {
			for( auto [n,t] : new_srcs_ ) if (t==FileTag::Dir) throw "new source dir "+n->name()+' '+git_clean_msg() ;
			for( auto [n,t] : old_srcs  ) if (t==FileTag::Dir) throw "old source dir "+n->name()+' '+git_clean_msg() ;
		}
		//
		for( Node d : src_dirs ) { if (old_src_dirs.contains(d)) old_src_dirs.erase(d) ; else new_src_dirs.insert(d) ; }
		//
		if ( !old_srcs && !new_srcs_ ) return false ;
		if (dynamic) {
			if (+new_srcs_) throw "new source "    +new_srcs_.begin()->first->name() ;                                                          // XXX : accept new sources if unknown
			if (+old_srcs ) throw "removed source "+old_srcs .begin()->first->name() ;                                                          // XXX : accept old sources if unknown
			FAIL() ;
		}
		//
		trace("srcs",'-',old_srcs.size(),'+',new_srcs_.size()) ;
		// commit
		for( bool add : {false,true} ) {
			::map<Node,FileTag> const& srcs = add ? new_srcs_ : old_srcs ;
			::vector<Node>             ss   ; ss.reserve(srcs.size()) ;                                                                         // typically, there are very few src dirs
			::vector<Node>             sds  ;                                                                                                   // .
			for( auto [n,t] : srcs ) if (t==FileTag::Dir) sds.push_back(n) ; else ss.push_back(n) ;
			//    vvvvvvvvvvvvvvvvvvvvvvvvvvvvv
			Node::s_srcs(false/*dirs*/,add,ss ) ;
			Node::s_srcs(true /*dirs*/,add,sds) ;
			//    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
		}
		{	Trace trace2 ;
			for( auto [n,t] : old_srcs     ) { Node(n)->mk_no_src()           ; trace2('-',t==FileTag::Dir?"dir":"",n) ; }
			for( Node  d    : old_src_dirs )        d ->mk_no_src()           ;
			for( auto [n,t] : new_srcs_    ) { Node(n)->mk_src(t            ) ; trace2('+',t==FileTag::Dir?"dir":"",n) ; }
			for( Node  d    : new_src_dirs )        d ->mk_src(FileTag::None) ;
		}
		_compile_srcs() ;
		// user report
		{	OFStream srcs_stream{AdminDirS+"manifest"s} ;
			for( auto [n,t] : srcs ) srcs_stream << n->name() << (t==FileTag::Dir?"/":"") <<'\n' ;
		}
		trace("done",srcs.size(),"srcs") ;
		return true ;
	}

	void invalidate_match() {
		MatchGen& match_gen = _rule_file.hdr() ;
		if (match_gen<NMatchGen) {
			match_gen++ ;                                   // increase generation, which automatically makes all nodes !match_ok()
			Trace("invalidate_match","new gen",match_gen) ;
			Rule::s_match_gen = match_gen ;
		} else {
			_collect_old_rules() ;
		}
	}

}
