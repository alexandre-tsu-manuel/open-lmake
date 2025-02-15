# This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
# Copyright (c) 2023 Doliam
# This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
# This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

'''
	Normally, job support functions defined here are implemented in C++ in lmake.clmake.
	However, in case the Python lib cannot be dynamically imported, this module provides a minimal fall back in pure Python.
'''

import os      as _os
import os.path as _osp

# provide minimal support in pure python
# XXX : provide full support

import subprocess as _sp
def _run(cmd_line,**kwds) :
	return _sp.check_output(cmd_line,universal_newlines=True,**kwds)
_bin_dir_s = _osp.dirname(_osp.dirname(_osp.dirname(__file__)))+'/bin/'
def _bin(f) : return _bin_dir_s+f
#
def depend(
	*args
,	critical=False , essential=False , ignore_error=False , required=True , ignore=False , stat_read_data=False
,	follow_symlinks=False , read=True , verbose=False
) :
	assert not verbose,'verbose is not supported without dynamic python librairy'
	cmd_line = [_bin('ldepend')]
	if     critical        : cmd_line.append('--critical'       )
	if     essential       : cmd_line.append('--essential'      )
	if     ignore_error    : cmd_line.append('--ignore-error'   )
	if not required        : cmd_line.append('--no-required'    )
	if     ignore          : cmd_line.append('--ignore'         )
	if     stat_read_data  : cmd_line.append('--stat-read-data' )
	if     follow_symlinks : cmd_line.append('--follow-symlinks')
	if not read            : cmd_line.append('--no-read'        )
	cmd_line += args
	_run(cmd_line)
def target(
	*args
,	essential=False , incremental=False , no_uniquify=False , no_warning=False , phony=False , ignore=False , no_allow=False , source_ok=False
,	follow_symlinks=False , write=True
) :
	cmd_line = [_bin('ltarget')]
	if     essential       : cmd_line.append('--essential'      )
	if     incremental     : cmd_line.append('--incremental'    )
	if     no_uniquify     : cmd_line.append('--no-uniquify'    )
	if     no_warning      : cmd_line.append('--no-warning'     )
	if     phony           : cmd_line.append('--phony'          )
	if     ignore          : cmd_line.append('--ignore'         )
	if     no_allow        : cmd_line.append('--no-allow'       )
	if     source_ok       : cmd_line.append('--source-ok'      )
	if     follow_symlinks : cmd_line.append('--follow-symlinks')
	if not write           : cmd_line.append('--no-write'       )
	cmd_line += args
	_run(cmd_line)
#
def decode    (file,ctx,code         ) : return _run((_bin('ldecode'    ),'-f',file,'-x',ctx,'-c',code        )          )
def encode    (file,ctx,val,min_len=1) : return _run((_bin('lencode'    ),'-f',file,'-x',ctx,'-l',str(min_len)),input=val)[:-1] # suppress terminating newline
def check_deps(                      ) :        _run((_bin('lcheck_deps'),                                    )          )
#
def get_autodep(      ) : return True                                                                                           # placeholder
def set_autodep(enable) : pass                                                                                                  # .
#
if 'ROOT_DIR' in _os.environ :
	root_dir = _os.environ['ROOT_DIR']
else :
	root_dir = _os.getcwd()
	while root_dir!='/' and not _osp.exists(root_dir+'/Lmakefile.py') : root_dir = _osp.dirname(root_dir)                       # avoid searching Lmakefile.py to avoid new dependency
	if not root_dir : del root_dir
#
autodeps = ()
if "$HAS_FUSE"     : autodeps += ('fuse'    ,)
if "$HAS_LD_AUDIT" : autodeps += ('ld_audit',)
autodeps += ('ld_preload','ld_preload_jemalloc','ptrace')
#
backends = ('local',)
if "$HAS_SGE"   : backends += ('sge'  ,)
if "$HAS_SLURM" : backends += ('slurm',)
