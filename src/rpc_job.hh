// This file is part of the lmake distribution (git@github.com:cesar-douady/open-lmake.git)
// Copyright (c) 2023 Doliam
// This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
// This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

#pragma once

#include "config.hh"

#include "disk.hh"
#include "hash.hh"
#include "lib.hh"
#include "serialize.hh"
#include "time.hh"

ENUM_2( BackendTag                     // PER_BACKEND : add a tag for each backend
,	Dflt    = Local
,	IsLocal = Local                    // <=IsLocal means backend launches jobs locally
,	Local
)

ENUM_1( DFlag                          // flags for deps
,	Private = Static                   // >=Private means flag cannot be seen in Lmakefile.py
//
,	Critical                           // if modified, ignore following deps
,	Essential                          // show when generating user oriented graphs
,	NoError                            // propagate error if dep is in error (Error instead of Err because name is visible from user)
,	Required                           // do not accept if dep cannot be built
//
,	Static                             // dep is static
,	Lnk                                // syscall sees link    content if dep is a link
,	Reg                                // syscall sees regular content if dep is regular
,	Stat                               // syscall sees inode   content (implied by other accesses)
)
static constexpr char DFlagChars[] = {
	'c'                                // Critical
,	's'                                // Essential
,	'e'                                // NoError
,	'r'                                // Required
//
,	'S'                                // Static
,	'L'                                // Lnk
,	'R'                                // Reg
,	'T'                                // Stat
} ;
static_assert(::size(DFlagChars)==+DFlag::N) ;
using DFlags = BitMap<DFlag> ;
constexpr DFlags StaticDFlags { DFlag::Essential , DFlag::Required , DFlag::Static } ; // used for static deps
constexpr DFlags AccessDFlags { DFlag::Lnk       , DFlag::Reg      , DFlag::Stat   } ;
constexpr DFlags DataDFlags   { DFlag::Lnk       , DFlag::Reg                      } ;
constexpr DFlags DfltDFlags   = AccessDFlags                                         ; // used with ldepend

ENUM( JobProc
,	None
,	Start
,	ReportStart
,	Continue       // req is killed but job is necessary for some other req
,	NotStarted     // req was killed before it actually started
,	ChkDeps
,	DepInfos
,	LiveOut
,	End
)

ENUM( Status       // result of job execution
,	New            // job was never run
,	Lost           // job was lost (it disappeared for an unknown reason)
,	Killed         // <=Killed means job was killed
,	ChkDeps        // dep check failed
,	Garbage        // <=Garbage means job has not run reliably
,	Ok             // job execution ended successfully
,	Frozen         // job behaves as a source
,	Err            // >=Err means job ended in error
,	ErrFrozen      // job is frozen in error
,	Timeout        // job timed out
,	EarlyErr       // job failed before even starting
)

ENUM_2( TFlag                          // flags for targets
,	RuleOnly = Incremental             // if >=RuleOnly, flag may only appear in rule
,	Private  = N                       // >=Private means flag cannot be seen in Lmakefile.py
//
,	Crc                                // generate a crc for this target (compulsery if Match)
,	Dep                                // reads not followed by writes trigger dependencies
,	Essential                          // show when generating user oriented graphs
,	Phony                              // unlinks are allowed (possibly followed by reads which are ignored)
,	SourceOk                           // ok to overwrite source files
,	Stat                               // inode accesses (stat-like) are not ignored
,	Write                              // writes are allowed (possibly followed by reads which are ignored)
//
,	Incremental                        // reads are allowed (before earliest write if any)
,	ManualOk                           // ok to overwrite manual files
,	Match                              // make target non-official (no match on it)
,	Star                               // target is a star target, even if no star stems
,	Warning                            // warn if target is unlinked and was generated by another rule
)
static constexpr char TFlagChars[] = {
	'c'                                // Crc
,	'd'                                // Dep
,	'e'                                // Essential
,	'f'                                // Phony
,	's'                                // SourceOk
,	't'                                // Stat
,	'w'                                // Write
,	'I'                                // Incremental
,	'M'                                // ManualOk
,	'T'                                // Match
,	'S'                                // Star
,	'W'                                // Warning
} ;
static_assert(::size(TFlagChars)==+TFlag::N) ;
using TFlags = BitMap<TFlag> ;
static constexpr TFlags DfltTFlags      { TFlag::Dep , TFlag::Stat , TFlag::Crc , TFlag::Match , TFlag::Warning , TFlag::Write } ; // default flags for targets
static constexpr TFlags UnexpectedTFlags{ TFlag::Dep , TFlag::Stat , TFlag::Incremental , TFlag::Star                          } ; // flags used for accesses that are not targets

static inline void chk(TFlags flags) {
	if (flags[TFlag::Match]) {
		if ( flags[TFlag::Dep] ) throw "cannot match on target and be a potential dep"s       ;
		if (!flags[TFlag::Crc] ) throw "cannot match on target without computing checksum"s   ;
	}
	if (flags[TFlag::Star]) {
		if (flags[TFlag::Phony]) throw "phony star targets not yet supported"s ;
	}
}

ENUM_1( JobReasonTag                   // see explanations in table below
,	HasNode = ClashTarget              // if >=HasNode, a node is associated
//
,	None
// with reason
,	ChkDeps
,	Cmd
,	Force
,	Garbage
,	Killed
,	Lost
,	New
,	OldError
,	Rsrcs
// with node
,	ClashTarget
,	DepChanged
,	DepNotReady
,	DepOutOfDate
,	NoTarget
,	PrevTarget
// with error
,	DepErr                             // if >=DepErr, job did not complete because of a dep
,	DepMissingStatic
,	DepMissingRequired
,	DepOverwritten
)
static constexpr const char* JobReasonTagStrs[] = {
	"no reason"                                            // None
// with reason
,	"dep check requires rerun"                             // ChkDeps
,	"command changed"                                      // Cmd
,	"job forced"                                           // Force
,	"job ran with unstable data"                           // Garbage
,	"job was killed"                                       // Killed
,	"job was lost"                                         // Lost
,	"job was never run"                                    // New
,	"job was in error"                                     // OldError
,	"resources changed and job was in error"               // Rsrcs
// with node
,	"multiple simultaneous writes"                         // ClashTarget
,	"dep changed"                                          // DepChanged
,	"dep not ready"                                        // DepNotReady
,	"dep out of date"                                      // DepOutOfDate
,	"target missing"                                       // NoTarget
,	"target previously existed"                            // PrevTarget
// with error
,	"dep in error"                                         // DepErr
,	"static dep missing"                                   // DepMissingStatic
,	"required dep missing"                                 // DepMissingRequired
,	"dep has been overwritten"                             // DepOverwritten
} ;
static_assert(::size(JobReasonTagStrs)==+JobReasonTag::N) ;

struct JobReason {
	friend ::ostream& operator<<( ::ostream& , JobReason const& ) ;
	using Tag = JobReasonTag ;
	// cxtors & casts
	JobReason(                   ) = default ;
	JobReason( Tag t             ) : tag{t}           { SWEAR( t< Tag::HasNode       ) ; }
	JobReason( Tag t , NodeIdx n ) : tag{t} , node{n} { SWEAR( t>=Tag::HasNode && +n ) ; }
	// accesses
	bool operator+() const { return +tag              ; }
	bool operator!() const { return !tag              ; }
	bool has_err  () const { return tag>=Tag::DepErr  ; }
	// services
	JobReason operator|(JobReason jr) const {
		if (   has_err()) return *this ;
		if (jr.has_err()) return jr    ;
		if (+*this      ) return *this ;
		/**/              return jr    ;
	}
	JobReason& operator|=(JobReason jr) { *this = *this | jr ; return *this ; }
	// data
	Tag     tag  = JobReasonTag::None ;
	NodeIdx node = 0                  ;
} ;

struct SubmitAttrs {
	friend ::ostream& operator<<( ::ostream& , SubmitAttrs const& ) ;
	// services
	SubmitAttrs& operator|=(SubmitAttrs const& other) {
		if      (      tag==BackendTag::Unknown) tag = other.tag ;
		else if (other.tag!=BackendTag::Unknown) SWEAR(tag==other.tag) ;
		pressure  = ::max(pressure,other.pressure) ;
		live_out |= other.live_out                 ;
		reason   |= other.reason                   ;
		return *this ;
	}
	SubmitAttrs operator|(SubmitAttrs const& other) const {
		SubmitAttrs res = *this ;
		res |= other ;
		return res ;
	}
	// data
	BackendTag        tag      = BackendTag::Unknown ;
	Time::CoarseDelay pressure = {}                  ;
	bool              live_out = false               ;
	JobReason         reason   = {}                  ;
} ;

struct JobStats {
	using Delay = Time::Delay ;
	// data
	Delay  cpu   ;
	Delay  job   ;                     // elapsed in job
	Delay  total ;                     // elapsed including overhead
	size_t mem   = 0 ;                 // in bytes
} ;

// for Dep recording in book-keeping, we want to derive from Node
// but if we derive from Node and have a field DepDigest, it is impossible to have a compact layout because of alignment constraints
// hence this solution : derive from a template argument
template<class B> struct DepDigestBase ;
template<class B> ::ostream& operator<<( ::ostream& , DepDigestBase<B> const& ) ;
template<class B> struct DepDigestBase : NoVoid<B> {
	friend ::ostream& operator<< <>( ::ostream& , DepDigestBase const& ) ;
	using Base = NoVoid<B>      ;
	using Date = Time::DiskDate ;
	using Crc  = Hash::Crc      ;
	//cxtors & casts
	DepDigestBase(                                       )                                                                {}
	DepDigestBase(          DFlags dfs , bool p          ) :           flags(dfs) , parallel{p}                           {}
	DepDigestBase(          DFlags dfs , bool p , Crc  c ) :           flags(dfs) , parallel{p} , is_date{No } , _crc {c} {}
	DepDigestBase(          DFlags dfs , bool p , Date d ) :           flags(dfs) , parallel{p} , is_date{Yes} , _date{d} {}
	DepDigestBase( Base b , DFlags dfs , bool p          ) : Base{b} , flags(dfs) , parallel{p}                           {}
	DepDigestBase( Base b , DFlags dfs , bool p , Crc  c ) : Base{b} , flags(dfs) , parallel{p} , is_date{No } , _crc {c} {}
	DepDigestBase( Base b , DFlags dfs , bool p , Date d ) : Base{b} , flags(dfs) , parallel{p} , is_date{Yes} , _date{d} {}
	//
	DepDigestBase(DepDigestBase const& dd) : DepDigestBase{dd,dd.flags,dd.parallel} {
		crc_date(dd) ;
		known   = dd.known   ;
		garbage = dd.garbage ;
	}
	~DepDigestBase() { clear_crc_date() ; }
	DepDigestBase& operator=(DepDigestBase const& dd) {
		(*this).~DepDigestBase() ;
		new(this) DepDigestBase{dd} ;
		return *this ;
	}
	// accesses
	bool has_crc       (      ) const {                       return is_date==No && +_crc ; }
	Crc  crc           (      ) const { SWEAR(is_date==No ) ; return _crc                 ; }
	Date date          (      ) const { SWEAR(is_date==Yes) ; return _date                ; }
	void crc           (Crc  c)       { if (is_date==Yes) _date.~Date() ; if (is_date!=No ) new(&_crc ) Crc {c} ; else _crc  = c ; is_date = No  ; }
	void date          (Date d)       { if (is_date==No ) _crc .~Crc () ; if (is_date!=Yes) new(&_date) Date{d} ; else _date = d ; is_date = Yes ; }
	void clear_crc_date(      )       {
		switch (is_date) {
			case No    : _crc .~Crc () ; break ;
			case Yes   : _date.~Date() ; break ;
			case Maybe :                 break ;
			default : FAIL(is_date) ;
		}
		is_date = Maybe ;
	}
	template<class X> void crc_date(DepDigestBase<X> const& dd) {
		switch (dd.is_date) {
			case No    : crc (dd.crc ()) ; break ;
			case Yes   : date(dd.date()) ; break ;
			case Maybe :                   break ;
			default : FAIL(dd.is_date) ;
		}
	}
	// services
	template<IsStream T> void serdes(T& s) {
		::serdes(s,flags   ) ;
		::serdes(s,parallel) ;
		if (::is_base_of_v<::istream,T>) {
			clear_crc_date() ;
			bool  kn ; ::serdes(s,kn) ; known   = kn ;
			bool  g  ; ::serdes(s,g ) ; garbage = g  ;
			Bool3 id ; ::serdes(s,id) ; is_date = id ;
		} else {
			bool  kn = known   ; ::serdes(s,kn) ;
			bool  g  = garbage ; ::serdes(s,g ) ;
			Bool3 id = is_date ; ::serdes(s,id) ;
		}
		switch (is_date) {
			case No    : ::serdes(s,_crc ) ; break ;
			case Yes   : ::serdes(s,_date) ; break ;
			case Maybe :                     break ;
			default : FAIL(is_date) ;
		}
	}
	// data
	DFlags flags      ;                // 8<=8 bits
	bool   parallel   = false ;        // 1<=8 bits
	bool   known   :1 = false ;        //    1 bit , dep was known (and thus done) before starting execution
	bool   garbage :1 = false ;        //    1 bit , if true <= file was not the same between the first and last access
	Bool3  is_date :2 = Maybe ;        //    2 bits, Maybe means no access : no date, no crc
private :
	union {
		Crc  _crc  ;                   // ~45<64 bits
		Date _date ;                   // ~45<64 bits
	} ;
} ;
template<class B> ::ostream& operator<<( ::ostream& os , DepDigestBase<B> const& dd ) {
	const char* sep = "" ;
	os << "D(" ;
	if constexpr (!::is_void_v<B>) { os <<sep<< static_cast<B const&>(dd) ; sep = "," ; }
	if           (+dd.flags      ) { os <<sep<< dd.flags                  ; sep = "," ; }
	if           (dd.parallel    ) { os <<sep<< "parallel"                ; sep = "," ; }
	if           (dd.known       ) { os <<sep<< "known"                   ; sep = "," ; }
	if           (dd.garbage     ) { os <<sep<< "garbage"                 ; sep = "," ; }
	switch (dd.is_date) {
		case No    : os <<sep<< dd._crc  ; sep = "," ; break ;
		case Yes   : os <<sep<< dd._date ; sep = "," ; break ;
		case Maybe :                                   break ;
		default : FAIL(dd.is_date) ;
	}
	return os <<')' ;
}

using DepDigest = DepDigestBase<void> ;

struct TargetDigest {
	friend ::ostream& operator<<( ::ostream& , TargetDigest const& ) ;
	using Crc  = Hash::Crc  ;
	// cxtors & casts
	TargetDigest(                                       ) = default ;
	TargetDigest( DFlags d , bool w , TFlags t , bool u ) : dfs{d} , write{w} , tfs{t} , crc{u?Crc::None:Crc::Unknown} {}
	// data
	DFlags dfs   ;                     // how target was accessed before it was written
	bool   write = false ;             // if true <=> file was written (and possibly further unlinked)
	TFlags tfs   ;
	Crc    crc   ;                     // if None <=> file was unlinked, if Unknown <=> file is idle (not written, not unlinked)
} ;

using AnalysisErr = ::vector<pair_s<NodeIdx>> ;

struct JobDigest {
	friend ::ostream& operator<<( ::ostream& , JobDigest const& ) ;
	// services
	template<IsStream T> void serdes(T& s) {
		::serdes(s,status      ) ;
		::serdes(s,targets     ) ;
		::serdes(s,deps        ) ;
		::serdes(s,analysis_err) ;
		::serdes(s,stderr      ) ;
		::serdes(s,stdout      ) ;
		::serdes(s,wstatus     ) ;
		::serdes(s,end_date    ) ;
		::serdes(s,stats       ) ;
	}
	// data
	Status                 status       = Status::New ;
	::vmap_s<TargetDigest> targets      = {}          ;
	::vmap_s<DepDigest   > deps         = {}          ;    // INVARIANT : sorted in first access order
	AnalysisErr            analysis_err = {}          ;
	::string               stderr       = {}          ;
	::string               stdout       = {}          ;
	int                    wstatus      = 0           ;
	Time::ProcessDate      end_date     = {}          ;
	JobStats               stats        = {}          ;
} ;

struct JobRpcReq {
	using P   = JobProc             ;
	using S   = Status              ;
	using SI  = SeqId               ;
	using JI  = JobIdx              ;
	using MDD = ::vmap_s<DepDigest> ;
	friend ::ostream& operator<<( ::ostream& , JobRpcReq const& ) ;
	// cxtors & casts
	JobRpcReq() = default ;
	JobRpcReq( P p , SI ui , JI j , ::string const& h , in_port_t pt           ) : proc{p} , seq_id{ui} , job{j} , host{h} , port  {pt                  } { SWEAR( p==P::Start                     ) ; }
	JobRpcReq( P p , SI ui , JI j ,                     S s                    ) : proc{p} , seq_id{ui} , job{j} ,           digest{.status=s           } { SWEAR( p==P::End && s<=S::Garbage      ) ; }
	JobRpcReq( P p ,         JI j ,                     S s , ::string&& e     ) : proc{p} ,              job{j} ,           digest{.status=s,.stderr{e}} { SWEAR( p==P::End && s>=S::Err          ) ; }
	JobRpcReq( P p , SI ui , JI j , ::string const& h , JobDigest const& d     ) : proc{p} , seq_id{ui} , job{j} , host{h} , digest{d                   } { SWEAR( p==P::End                       ) ; }
	JobRpcReq( P p , SI ui , JI j , ::string const& h , ::string_view const& t ) : proc{p} , seq_id{ui} , job{j} , host{h} , txt   {t                   } { SWEAR( p==P::LiveOut                   ) ; }
	JobRpcReq( P p , SI ui , JI j , ::string const& h , MDD const& ds          ) : proc{p} , seq_id{ui} , job{j} , host{h} , digest{.deps=ds            } { SWEAR( p==P::ChkDeps || p==P::DepInfos ) ; }
	// services
	template<IsStream T> void serdes(T& s) {
		if (::is_base_of_v<::istream,T>) *this = JobRpcReq() ;
		::serdes(s,proc  ) ;
		::serdes(s,seq_id) ;
		::serdes(s,job   ) ;
		switch (proc) {
			case P::Start    : ::serdes(s,host) ; ::serdes(s,port) ; break ;
			case P::LiveOut  : ::serdes(s,txt) ;                     break ;
			case P::ChkDeps  :
			case P::DepInfos :
			case P::End      : ::serdes(s,digest) ;                  break ;
			default          : ;
		}
	}
	// data
	P         proc   = P::None ;
	SI        seq_id = 0       ;
	JI        job    = 0       ;
	::string  host   ;                 // if proc==Start
	in_port_t port   = 0       ;       // if proc==Start
	JobDigest digest ;                 // if proc==ChkDeps || DepInfos || End
	::string  txt    ;                 // if proc==LiveOut
} ;

struct TargetSpec {
	friend ::ostream& operator<<( ::ostream& , TargetSpec const& ) ;
	// cxtors & casts
	TargetSpec( ::string const& p={} , bool ins=false , TFlags f={} , ::vector<VarIdx> c={} ) : pattern{p} , is_native_star{ins} , flags{f} , conflicts{c} {
		if (is_native_star) SWEAR(flags[TFlag::Star]) ;
	}
	template<IsStream S> void serdes(S& s) {
		::serdes(s,pattern       ) ;
		::serdes(s,is_native_star) ;
		::serdes(s,flags         ) ;
		::serdes(s,conflicts     ) ;
	}
	// services
	bool operator==(TargetSpec const&) const = default ;
	// data
	::string         pattern        ;
	bool             is_native_star = false ;
	TFlags           flags          ;
	::vector<VarIdx> conflicts      ;                      // the idx of the previous targets that may conflict with this one
} ;

ENUM_2( AutodepMethod
,	Ld   = LdAudit                                         // >=Ld means a lib is pre-loaded (through LD_AUDIT or LD_PRELOAD)
,	Dflt =                                                 // by default, use most reliable available method
		HAS_PTRACE   ? AutodepMethod::Ptrace
	:	HAS_LD_AUDIT ? AutodepMethod::LdAudit
	:	               AutodepMethod::LdPreload
,	None
,	Ptrace
,	LdAudit
,	LdPreload
)

struct JobRpcReply {
	friend ::ostream& operator<<( ::ostream& , JobRpcReply const& ) ;
	using Crc  = Hash::Crc ;
	using Proc = JobProc   ;
	// cxtors & casts
	JobRpcReply(                                                    ) = default ;
	JobRpcReply( Proc p                                             ) : proc{p}             {                               }
	JobRpcReply( Proc p , Bool3 o                                   ) : proc{p} , ok{o}     { SWEAR(proc==Proc::ChkDeps ) ; }
	JobRpcReply( Proc p , ::vector<pair<Bool3/*ok*/,Crc>> const& is ) : proc{p} , infos{is} { SWEAR(proc==Proc::DepInfos) ; }
	// services
	template<IsStream S> void serdes(S& s) {
		if (is_base_of_v<::istream,S>) *this = JobRpcReply() ;
		::serdes(s,proc) ;
		switch (proc) {
			case Proc::None     :                     break ;
			case Proc::DepInfos : ::serdes(s,infos) ; break ;
			case Proc::ChkDeps  : ::serdes(s,ok   ) ; break ;
			case Proc::Start :
				::serdes(s,addr            ) ;
				::serdes(s,auto_mkdir      ) ;
				::serdes(s,chroot          ) ;
				::serdes(s,cmd             ) ;
				::serdes(s,cwd_s           ) ;
				::serdes(s,env             ) ;
				::serdes(s,hash_algo       ) ;
				::serdes(s,ignore_stat     ) ;
				::serdes(s,interpreter     ) ;
				::serdes(s,job_tmp_dir     ) ;
				::serdes(s,keep_tmp        ) ;
				::serdes(s,kill_sigs       ) ;
				::serdes(s,live_out        ) ;
				::serdes(s,lnk_support     ) ;
				::serdes(s,local_mrkr      ) ;
				::serdes(s,method          ) ;
				::serdes(s,remote_admin_dir) ;
				::serdes(s,root_dir        ) ;
				::serdes(s,small_id        ) ;
				::serdes(s,static_deps     ) ;
				::serdes(s,stdin           ) ;
				::serdes(s,stdout          ) ;
				::serdes(s,targets         ) ;
				::serdes(s,timeout         ) ;
			break ;
			default : FAIL(proc) ;
		}
	}
	// data
	Proc                      proc             = Proc::None          ;
	in_addr_t                 addr             = 0                   ;         // proc == Start   , the address at which server can contact job, it is assumed that it can be used by subprocesses
	bool                      auto_mkdir       = false               ;         // proc == Start   , if true <=> auto mkdir in case of chdir
	::string                  chroot           ;                               // proc == Start
	::string                  cmd              ;                               // proc == Start
	::string                  cwd_s            ;                               // proc == Start
	::vmap_ss                 env              ;                               // proc == Start
	Hash::Algo                hash_algo        = Hash::Algo::Unknown ;         // proc == Start
	bool                      ignore_stat      = false               ;         // proc == Start   , if true <=> stat-like syscalls do not trigger dependencies
	::vector_s                interpreter      ;                               // proc == Start   , actual interpreter used to execute cmd
	::string                  job_tmp_dir      ;                               // proc == Start
	bool                      keep_tmp         = false               ;         // proc == Start
	vector<uint8_t>           kill_sigs        ;                               // proc == Start
	bool                      live_out         = false               ;         // proc == Start
	LnkSupport                lnk_support      = LnkSupport   ::None ;         // proc == Start
	::string                  local_mrkr       ;                               // proc == Start
	AutodepMethod             method           = AutodepMethod::None ;         // proc == Start
	::string                  remote_admin_dir ;                               // proc == Start
	::string                  root_dir         ;                               // proc == Start
	SmallId                   small_id         = 0                   ;         // proc == Start
	::vector_s                static_deps      ;                               // proc == Start   , deps that may clash with targets
	::string                  stdin            ;                               // proc == Start
	::string                  stdout           ;                               // proc == Start
	::vector<TargetSpec>      targets          ;                               // proc == Start
	Time::Delay               timeout          ;                               // proc == Start
	Bool3                     ok               = Maybe               ;         // proc == ChkDeps , if No <=> deps in error, if Maybe <=> deps not ready
	::vector<pair<Bool3,Crc>> infos            ;                               // proc == DepInfos
} ;

ENUM( JobExecRpcProc
,	None
,	ChkDeps
,	CriticalBarrier
,	DepInfos
,	Heartbeat
,	Kill
,	Tmp                                // write activity in tmp has been detected (hence clean up is required)
,	Trace                              // no algorithmic info, just for tracing purpose
,	Access
)

struct JobExecRpcReq {
	friend ::ostream& operator<<( ::ostream& , JobExecRpcReq const& ) ;
	// make short lines
	using P  = JobExecRpcProc    ;
	using PD = Time::ProcessDate ;
	using DD = Time::DiskDate    ;
	//
	struct AccessInfo {                                                        // order is read, then write, then unlink
		friend ::ostream& operator<<( ::ostream& , AccessInfo const& ) ;
		// accesses
		bool idle() const { return !write && !unlink ; }
		// services
		bool operator==(AccessInfo const& ai) const = default ;                // XXX : why is this necessary at all ?!?
		AccessInfo operator|(AccessInfo const& ai) const {                     // *this, then other
			AccessInfo res = *this ;
			res |= ai ;
			return res ;
		}
		AccessInfo& operator|=(AccessInfo const& ai) {
			update(ai,Yes) ;
			return *this ;
		}
		// update this with access from ai, which may be before or after this (or between the read part and the write part is after==Maybe)
		void update( AccessInfo const& , Bool3 after ) ;
		// data
		DFlags dfs     = {}    ;       // if +dfs <=> files are read
		bool   write   = false ;       // if true <=> files are written
		TFlags neg_tfs = {}    ;       // if write, removed TFlags
		TFlags pos_tfs = {}    ;       // if write, added   TFlags
		bool   unlink  = false ;       // if true <=> files are unlinked
	} ;
	// statics
private :
	static ::vmap_s<DD> _s_mk_mdd(::vector_s const& fs) { ::vmap_s<DD> res ; for( ::string const& f : fs ) res.emplace_back(       f ,DD()) ; return res ; }
	static ::vmap_s<DD> _s_mk_mdd(::vector_s     && fs) { ::vmap_s<DD> res ; for( ::string      & f : fs ) res.emplace_back(::move(f),DD()) ; return res ; }
	// cxtors & casts
public :
	JobExecRpcReq(                ::string const& c={} ) :                     comment{c} {                       }
	JobExecRpcReq( P p , bool s , ::string const& c={} ) : proc{p} , sync{s} , comment{c} { SWEAR(!has_files()) ; }
	JobExecRpcReq( P p ,          ::string const& c={} ) : proc{p} ,           comment{c} { SWEAR(!has_files()) ; }
	//
	JobExecRpcReq( P p , ::vmap_s<DD>&& fs , DFlags dfs , bool nf , ::string const& c={} ) :
		proc     {p         }
	,	sync     {true      }
	,	no_follow{nf        }
	,	files    {::move(fs)}
	,	info     {.dfs=dfs  }
	,	comment  {c         }
	{ SWEAR(p==P::DepInfos) ; }
	JobExecRpcReq( P p , ::vector_s  && fs , DFlags dfs , bool nf , ::string const& c={} ) :
		proc     {p                    }
	,	sync     {true                 }
	,	auto_date{true                 }
	,	no_follow{nf                   }
	,	files    {_s_mk_mdd(::move(fs))}
	,	info     {.dfs=dfs             }
	,	comment  {c                    }
	{ SWEAR(p==P::DepInfos) ; }
	//
	JobExecRpcReq( P p , ::vmap_s<DD>&& fs , bool ad , AccessInfo const& ai , bool nf , bool s , ::string const& c={} ) :
		proc     {p         }
	,	sync     {s         }
	,	auto_date{ad        }
	,	no_follow{nf        }
	,	files    {::move(fs)}
	,	info     {ai        }
	,	comment  {c         }
	{ SWEAR(p==P::Access) ; }
	JobExecRpcReq( P p , ::vmap_s<DD>&& fs , AccessInfo const& ai ,           bool s , ::string const& c={} ) : JobExecRpcReq{p,          ::move(fs) ,false/*audo_date*/,ai,false,s    ,c} {}
	JobExecRpcReq( P p , ::vmap_s<DD>&& fs , AccessInfo const& ai ,                    ::string const& c={} ) : JobExecRpcReq{p,          ::move(fs) ,false/*audo_date*/,ai,false,false,c} {}
	JobExecRpcReq( P p , ::vector_s  && fs , AccessInfo const& ai , bool nf , bool s , ::string const& c={} ) : JobExecRpcReq{p,_s_mk_mdd(::move(fs)),true /*audo_date*/,ai,nf   ,s    ,c} {}
	JobExecRpcReq( P p , ::vector_s  && fs , AccessInfo const& ai , bool nf ,          ::string const& c={} ) : JobExecRpcReq{p,_s_mk_mdd(::move(fs)),true /*audo_date*/,ai,nf   ,false,c} {}
	//
	bool has_files() const { return proc==P::DepInfos || proc==P::Access ; }
	// services
public :
	template<IsStream T> void serdes(T& s) {
		if (::is_base_of_v<::istream,T>) *this = JobExecRpcReq() ;
		/**/              ::serdes(s,proc     ) ;
		/**/              ::serdes(s,date     ) ;
		/**/              ::serdes(s,sync     ) ;
		if (!has_files()) return ;
		/**/              ::serdes(s,auto_date) ;
		/**/              ::serdes(s,no_follow) ;
		/**/              ::serdes(s,files    ) ;
		/**/              ::serdes(s,info     ) ;
		/**/              ::serdes(s,comment  ) ;
	}
	// data
	P            proc      = P::None     ;
	PD           date      = PD::s_now() ;                 // access date to reorder accesses during analysis
	bool         sync      = false       ;
	bool         auto_date = false       ;                 // if has_files(), if true <=> files must be solved and dates added by probing disk (for autodep internal use, not to be sent to job_exec)
	bool         no_follow = false       ;                 // if files has yet to be made real, whether links should not be followed
	::vmap_s<DD> files     ;                               // file date when accessed with +dfs and !auto_date to identify content
	AccessInfo   info      ;
	::string     comment   ;
} ;

struct JobExecRpcReply {
	friend ::ostream& operator<<( ::ostream& , JobExecRpcReply const& ) ;
	using Proc = JobExecRpcProc ;
	using Crc  = Hash::Crc      ;
	// cxtors & casts
	JobExecRpcReply(                                                    ) = default ;
	JobExecRpcReply( Proc p                                             ) : proc{p}             { SWEAR( proc!=Proc::ChkDeps && proc!=Proc::DepInfos ) ; }
	JobExecRpcReply( Proc p , Bool3 o                                   ) : proc{p} , ok{o}     { SWEAR( proc==Proc::ChkDeps                         ) ; }
	JobExecRpcReply( Proc p , ::vector<pair<Bool3/*ok*/,Crc>> const& is ) : proc{p} , infos{is} { SWEAR( proc==Proc::DepInfos                        ) ; }
	JobExecRpcReply( JobRpcReply const& jrr ) {
		switch (jrr.proc) {
			case JobProc::None     :                        proc = Proc::None     ;                     break ;
			case JobProc::ChkDeps  : SWEAR(jrr.ok!=Maybe) ; proc = Proc::ChkDeps  ; ok    = jrr.ok    ; break ;
			case JobProc::DepInfos :                        proc = Proc::DepInfos ; infos = jrr.infos ; break ;
			default : FAIL(jrr.proc) ;
		}
	}
	// services
	template<IsStream S> void serdes(S& s) {
		if (::is_base_of_v<::istream,S>) *this = JobExecRpcReply() ;
		::serdes(s,proc) ;
		switch (proc) {
			case Proc::ChkDeps  : ::serdes(s,ok   ) ; break ;
			case Proc::DepInfos : ::serdes(s,infos) ; break ;
			default : ;
		}
	}
	// data
	Proc                            proc  = Proc::None ;
	Bool3                           ok    = Maybe      ;   // if proc==ChkDeps
	::vector<pair<Bool3/*ok*/,Crc>> infos ;                // if proc==DepInfos
} ;

struct JobInfoStart {
	friend ::ostream& operator<<( ::ostream& , JobInfoStart const& ) ;
	// services
	template<IsStream T> void serdes(T& s) {
		::serdes(s,eta         ) ;
		::serdes(s,submit_attrs) ;
		::serdes(s,rsrcs       ) ;
		::serdes(s,pre_start   ) ;
		::serdes(s,start       ) ;
	}
	// data
	Time::ProcessDate eta          = {} ;
	SubmitAttrs       submit_attrs = {} ;
	::vmap_ss         rsrcs        = {} ;
	JobRpcReq         pre_start    = {} ;
	JobRpcReply       start        = {} ;
} ;

using JobInfoEnd = JobRpcReq ;
