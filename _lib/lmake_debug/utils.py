# This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
# Copyright (c) 2023 Doliam
# This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
# This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

import os
import os.path as osp
import re

import lmake

from lmake.utils import multi_strip

no_quote     = re.compile(r'[\w+,-./:=@^]*')
single_quote = re.compile(r"[^']*"         )
double_quote = re.compile(r'[^!"$\\`]*'    )
def mk_shell_str(s) :
	if no_quote    .fullmatch(s) : return     s
	if single_quote.fullmatch(s) : return "'"+s+"'"
	if double_quote.fullmatch(s) : return '"'+s+'"'
	else                         : return "'"+s.replace("'",r"'\''")+"'"

def add_nl(s) :
	if not s or s[-1]=='\n' : return s
	else                    : return s+'\n'

def no_nl(s) :
	if s and s[-1]=='\n' : return s[:-1]
	else                 : return s

# XXX : handle ROOT_DIR and TMPDIR in case of views
class Job :

	def __init__(self,attrs) :
		for k,v in attrs.items() : setattr(self,k,v)

	#
	# functions for generating cmd file
	#

	def gen_shebang(self) :
		# shebang is not strictly necessary, but at least, it allows editors (e.g. vi) to do syntax coloring
		res = f"#!{' '.join(self.interpreter)}\n"
		if len(self.interpreter)>2 or len(res)>256 :                                                    # inform user we do not use the sheebang line if it actually does not work ...
			res += '# the shebang line above is informative only, interpreter is called explicitly\n' ; # ... just so that it gets no headache wondering why it works with a apparently buggy line
		return res

	def gen_cmd(self) :
		return (
			self.gen_shebang()
		+	add_nl(self.preamble)
		+	add_nl(self.cmd     )
		)

	def interpreter_line(self) :
		return ' '.join(mk_shell_str(c) for c in self.interpreter)

	#
	# functions for generating script file
	#

	def gen_pre_actions(self) :
		res = ''
		if any(a=='uniquify' for a in self.pre_actions.values()) :
			res += multi_strip('''
				uniquify() {
					if [ -f "$1" -a $(stat -c%h "$1" 2>/dev/null||echo 0) -gt 1 ] ; then
						echo warning : uniquify "$1" >&2
						mv "$1" "$1.$$" ; cp -p "$1.$$" "$1" ; rm -f "$1.$$"
					fi
				}
			''')
		if any(a=='unlink_warning' for a in self.pre_actions.values()) :
			res += multi_strip('''
				rm_warning() {
					if [ -f "$1" ] ; then
						echo warning : rm "$1" >&2
						rm -f "$1"
					fi
				}
			''')
		for f,a in self.pre_actions.items() :
			f = mk_shell_str(f)
			if   a=='mkdir'          : res += f'mkdir -p   {f}\n'
			elif a=='rmdir'          : res += f'rmdir      {f} 2>/dev/null\n'
			elif a=='unlink'         : res += f'rm -f      {f}\n'
			elif a=='unlink_warning' : res += f'rm_warning {f}\n'
			elif a=='uniquify'       : res += f'uniquify   {f}\n'
		return res

	def gen_tmp_dir(self) :
		return multi_strip(f'''
			export     TMPDIR={mk_shell_str(lmake.root_dir+'/'+self.debug_dir+'/tmp')}
			rm -rf   "$TMPDIR"
			mkdir -p "$TMPDIR"
		''')

	def starter(self,*args) :
		autodep            = f'{osp.dirname(osp.dirname(osp.dirname(__file__)))}/bin/autodep'
		keep_env           = self.keep_env + ('DISPLAY','LMAKE_HOME','LMAKE_SHLVL','TMPDIR','XAUTHORITY','XDG_RUNTIME_DIR')
		env                = dict(self.env)
		env['ROOT_DIR'   ] = mk_shell_str(lmake.root_dir  )
		env['SEQUENCE_ID'] = str         (self.sequence_id)
		env['SMALL_ID'   ] = str         (self.small_id   )
		#
		preamble  = 'export LMAKE_HOME="$HOME"\n'
		preamble += 'export LMAKE_SHLVL="${SHLVL:-1}"\n'
		preamble += 'export XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}"\n'
		#
		res = mk_shell_str(autodep)
		if True             : res += f' -m{self.autodep_method                     }'
		if self.auto_mkdir  : res +=  ' -a'
		if self.chroot_dir  : res += f' -c{mk_shell_str(self.chroot_dir           )}'
		if self.cwd         : res += f' -d{mk_shell_str(self.cwd                  )}'
		if self.env         : res += f' -e{mk_shell_str(repr(env)                 )}'
		if self.ignore_stat : res +=  ' -i'
		if True             : res += f' -k{mk_shell_str(repr(keep_env)            )}'
		if True             : res += f' -l{self.link_support                       }'
		if True             : res += f" -o{mk_shell_str(self.debug_dir+'/accesses')}"
		if self.root_view   : res += f' -r{mk_shell_str(self.root_view            )}'
		if self.source_dirs : res += f' -s{mk_shell_str(self.source_dirs          )}'
		if self.tmp_view    : res += f' -t{mk_shell_str(self.tmp_view             )}'
		if self.views       : res += f' -v{mk_shell_str(repr(self.views)          )}'
		if self.views       : res += f" -w{mk_shell_str(debug_dir+'/work'         )}" # necessary in case a view is an overlay
		if args             : res += ''.join(' '+x for x in args)                     # must be before redirections to files if args contains redirections
		if self.stdin       : res += f' <{mk_shell_str(self.stdin )}'
		if self.stdout      : res += f' >{mk_shell_str(self.stdout)}'
		#
		return preamble,res

	def gen_init(self) :
		res = multi_strip(f'''
			#!/bin/bash
			cd {mk_shell_str(lmake.root_dir)}
		''')
		return res

	def gen_shell_cmd( self , trace=False , enter=False , **kwds ) :
		res = ''
		if True            : res += self.gen_shebang()
		if True            : res += add_nl(self.preamble)
		if trace and enter : res += '(\n'
		if trace           : res += 'set -x\n'
		if True            : res += add_nl(self.cmd)
		if trace and enter : res += ')\n'
		if enter           :
			res += multi_strip(f'''
				export HOME="$LMAKE_HOME"   ; unset LMAKE_HOME
				export SHLVL="$LMAKE_SHLVL" ; unset LMAKE_SHLVL
				[ "$LMAKE_DEBUG_STDIN"  ] && exec <"$LMAKE_DEBUG_STDIN"
				[ "$LMAKE_DEBUG_STDOUT" ] && exec >"$LMAKE_DEBUG_STDOUT"
				exec {self.interpreter_line()} -i
			''')
		return res

	def gen_py_cmd( self , runner=None , **kwds ) :
		cmd = no_nl(self.cmd)
		if not runner :
			cmd = cmd+'\n'
		else :
			assert cmd[-1]==')'                                                   # cmd must be of the form func(args,...) with 0 or more args
			func,args = cmd.split('(',1)
			args = args[:-1]
			func_args = func
			if args : func_args = f'{func},{args}'                                # generate func,args,...
			else    : func_args = func                                            # handle no args case
			cmd = multi_strip(f'''
				import sys as lmake_sys
				lmake_sys.path.append({lmake.root_dir+'/lib'!r})                  # ensure lmake lib is accessible
				from {runner} import run_py as lmake_runner
				lmake_sys.path.pop()                                              # restore
				lmake_runner({self.debug_dir!r},{self.static_deps!r},{func_args})
			''')
		return (
			self.gen_shebang()
		+	add_nl(self.preamble)
		+	cmd
		)

	def cmd_file(self) :
		return self.debug_dir+'/cmd'

	def gen_start_line(self) :
		preamble,line = self.starter(*(mk_shell_str(c) for c in self.interpreter),mk_shell_str(self.cmd_file()))
		return preamble+line+'\n'                                                                                # do not use exec to launch autodep so as to keep stdin & stdout open

	def gen_lmake_env(self) :
		def add_env(key,val) :
			nonlocal res
			res           += f'export {key}={val}\n'
			self.keep_env += (key,)
		res = ''
		if True        : add_env('LMAKE_DEBUG_KEY'   ,mk_shell_str(self.key))
		if self.stdin  : add_env('LMAKE_DEBUG_STDIN' ,'/proc/$$/fd/0'       )
		if self.stdout : add_env('LMAKE_DEBUG_STDOUT','/proc/$$/fd/1'       )
		return res

	def gen_preamble(self) :
		res  = self.gen_init       ()
		res += self.gen_pre_actions()
		res += self.gen_tmp_dir    ()
		res += self.gen_lmake_env  ()
		return res

	def write_cmd(self,**kwds) :
		if self.is_python : cmd = self.gen_py_cmd   (**kwds)
		else              : cmd = self.gen_shell_cmd(**kwds)
		cmd_file = self.cmd_file()
		os.makedirs(osp.dirname(cmd_file),exist_ok=True)
		open(cmd_file,'w').write(cmd)

	def gen_script(self,**kwds) :
		self.write_cmd(**kwds)
		res  = self.gen_preamble  ()
		res += self.gen_start_line()
		return res