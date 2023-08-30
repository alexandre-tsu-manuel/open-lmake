# This file is part of the lmake distribution (git@github.com:cesar-douady/open-lmake.git)
# Copyright (c) 2023 Doliam
# This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
# This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

import os
import subprocess as sp
import sys
import time

import lmake

autodeps = []
if lmake.has_ptrace     : autodeps.append('ptrace'    )
if lmake.has_ld_audit   : autodeps.append('ld_audit'  )
if lmake.has_ld_preload : autodeps.append('ld_preload')

if getattr(sys,'reading_makefiles',False) :

	import step

	lmake.sources = (
		'Lmakefile.py'
	,	'step.py'
	)

	lmake.config.link_support = step.link_support

	class Base(lmake.Rule) :
		stems = { 'File' : r'.*' }

	class Delay(Base) :
		target = 'dly'
		cmd    = 'sleep 0.5'

	class Src(Base) :
		target = 'hello'
		dep    = 'dly'                                                         # ensure hello construction does not start too early, so that we are sure that we have may_rerun messages, not rerun
		cmd    = f'echo hello.{step.p>=2}.{step.link_support}'

	for ad in autodeps :
		class CpySh(Base) :
			name    = f'cpy-sh-{ad}'
			target  = f'{{File}}.sh.{ad}.{step.link_support}.cpy'
			autodep = ad
			cmd = '''
				from_server=$(ldepend -v {File} | awk '{{print $2}}')
				expected=$(   xxhsum     {File}                     )
				echo $from_server
				[ $from_server = $expected ] || echo expected $expected got $from_server >&2
			'''
		class CpyPy(Base) :
			name    = f'cpy-py-{ad}'
			target  = f'{{File}}.py.{ad}.{step.link_support}.cpy'
			autodep = ad
			def cmd() :
				from_server = lmake.depend(File,verbose=True)[File][1]
				expected    = sp.check_output(('xxhsum',File),universal_newlines=True).strip()
				assert from_server==expected,f'expected {expected} got {from_server}'
				print(from_server)

else :

	import ut

	n_ads = len(autodeps)

	for ls in ('none','file','full') :
		for p in range(3) :
			print(f'p={p!r}\nlink_support={ls!r}',file=open('step.py','w'))
			ut.lmake(
				*( f'hello.{interp}.{ad}.{ls}.cpy' for interp in ('sh','py') for ad in autodeps )
			,	may_rerun=(p==0)*2*n_ads , done=(p==0 and ls=='none')+(p!=1)+(p!=1)*2*n_ads , steady=(p==0 and ls!='none')
			)
