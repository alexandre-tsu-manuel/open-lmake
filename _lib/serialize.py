# This file is part of the open-lmake distribution (git@github.com:cesar-douady/open-lmake.git)
# Copyright (c) 2023 Doliam
# This program is free software: you can redistribute/modify under the terms of the GPL-v3 (https://www.gnu.org/licenses/gpl-3.0.html).
# This program is distributed WITHOUT ANY WARRANTY, without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

import builtins
import dis
import inspect
import os.path as osp
import pickle
import re
import sys
import types

class f_str(str) : pass                                                        # used as a marker to generate an f-string as source

__all__ = ('get_src','get_code_ctx')                                           # everything else is private

comment_re = re.compile(r'^\s*(#.*)?$')

_Code = (lambda:None).__code__.__class__

def get_src(*args,no_imports=(),ctx=(),force=False,decorator=None,root_dir=None) :
	'''
		get a source text that reproduce args :
		- args must be composed of named objects such as functions or classes or dicts mapping names to values.
		- no_imports is a list of modules or module names that must not be imported in the resulting source.
		- ctx is a list of dict or set to get indirect values from. If found in a set, no value is generated.
		- if force is true, args are guaranteed to be imported by value (i.e. they are not imported). Dependencies can be imported, though.
		- if decorator is true, use it to decorate all generated functions
		- if root_dir is provided, source filename debug info are reported relative to this directory
		The return value is (source,names) where :
			- source is the source text that reproduces args
			- names is the set of names found in sets in ctx
			- debug_info contains a dict mapping generated function names to (module,qualname,file,firstlineno)
	'''
	s = Serialize(no_imports,ctx,decorator,root_dir)
	for a in args :
		if isinstance(a,dict) :
			for k,v in a.items() : s.val_src(k,v,force)
		else :
			s.val_src(None,a,force=force)
	return s.get_src()

def get_expr(expr,*,no_imports=(),ctx=(),force=False,call_callables=False) :
	'''
		get an expression text that reproduce expr :
		- expr can be any object
		- no_imports is a list of modules or module names that must not be imported in the resulting source.
		- ctx is a list of dict or set to get indirect values from. If found in a set, no value is generated.
		- if force is true, args are guaranteed to be imported by value (i.e. they are not imported). Dependencies can be imported, though.
		The return value is (source,ctx,names) where :
			- source is the source text that reproduces expr as an expression
			- ctx is as ource text that reproduces the environment in which to evaluate source
			- names is the list of names found in sets in ctx
	'''
	s = Serialize(no_imports,ctx)
	src = s.expr_src(expr,force=force,call_callables=call_callables)
	return (src,*s.get_src())

def get_code_ctx(*args,no_imports=(),ctx=()) :
	'''
		get a source text that provides the necessary context to evaluate args :
		- args must be composed of code objects
		- no_imports is a list of modules or module names that must not be imported in the resulting source
		- ctx is a list of dict or set to get indirect values from. If found in a set, no value is generated
		the return value is (source,names) where :
			- source is the source text that provides the necessary context to evaluate args
			- names is the list of names found in sets in ctx
	'''
	s = Serialize(no_imports,ctx)
	for a in args :
		if not isinstance(a,types.CodeType) : raise TypeError(f'args must be code, not {a.__class__.__name__}')
		for glb_var in s.get_glbs(a) : s.gather_ctx(glb_var)
	return s.get_src()

def _mk_f_string(s) :
	if not s                            : return "f''"
	if "'"   not in s and '\n' not in s : return "f'"  +s+"'"
	if '"'   not in s and '\n' not in s : return 'f"'  +s+'"'
	if "'''" not in s and s[-1]!="'"    : return "f'''"+s+"'''"
	if '"""' not in s and s[-1]!='"'    : return 'f"""'+s+'"""'
	if "'''" not in s and s[-1]=="'"    : return "f'''"+s[:-1]+"\\''''"           # this \ is certainly outside {}, hence f-string is still valid
	if '"""' not in s and s[-1]=='"'    : return 'f"""'+s[:-1]+'\\""""'           # .
	else                                : return 'f'+repr(s)                      # hope that repr will not insert \ within {}

end_liness = {}
srcs       = {}
def _analyze(filename) :
	if filename in end_liness : return
	srcs      [filename] = lines          = open(filename).read().splitlines()
	end_liness[filename] = file_end_lines = {}
	for start_lineno in range(len(lines)) :
		start_line = lines[start_lineno]
		def_pos    = start_line.find('def')
		if def_pos==-1 : continue                                              # not function def here
		prefix = start_line[:def_pos]
		if prefix and not prefix.isspace() : continue                          # if def is not at start of line : not a function def either
		if start_lineno>0 :
			prev_line = lines[start_lineno-1]
			if prev_line and not prev_line.isspace() and prev_line.startswith(prefix) and prev_line[def_pos]=='@' :
				file_end_lines[start_lineno] = None                            # cannot handle decorators in serializer as we generally cannot reproduce the object
		candidate = None
		for lineno in range(start_lineno+1,len(lines)) :                       # XXX : handle misaligned (), [] & {} and multi-lines '''string''' & """string"""
			line = lines[lineno]
			if comment_re.match(line) :
				if not candidate : candidate = lineno                          # manage to exclude comment lines at end of funcs
				continue
			if line.startswith(prefix) and line[def_pos].isspace() :
				candidate = None
				continue
			end_lineno = candidate or lineno
			break
		else :
			end_lineno = candidate or len(lines)
		file_end_lines[start_lineno] = end_lineno

class Serialize :
	InSet = object()                                                           # a marker to mean that we have no value as name was found in a set (versus in a dict) in the context list
	def __init__(self,no_imports,ctx,decorator=None,root_dir=None) :
		self.seen            = {}
		self.src             = []
		self.in_sets         = set()
		self.ctx             = list(ctx)
		self.decorator       = decorator
		self.root_dir        = root_dir
		self.debug_info      = {}
		if isinstance(no_imports,str) : self.by_values =    {no_imports}
		else                          : self.by_values = set(no_imports)

	@staticmethod
	def has_repr(val,avoid=None) :
		try :
			if avoid is None : avoid = set()                                   # avoid is used to detect loops : loops have no repr (i.e. the repr output does not represent to object)
			cls = val.__class__
			if val in (None,...)                         : return True
			if cls is f_str                              : return False        # cannot use a simple repr call to generate an f-string
			if cls in (bool,int,float,complex,str,bytes) : return True
			val_id = id(val)
			if val_id in avoid : raise RuntimeError()
			avoid.add(val_id)
			if cls in (tuple,list,set) : return all( Serialize.has_repr(v,avoid)                                 for v   in val         )
			if cls is  dict            : return all( Serialize.has_repr(k,avoid) and Serialize.has_repr(v,avoid) for k,v in val.items() )
			avoid.discard(val_id)
			return False
		except RuntimeError :
			if not avoid : return False
			else         : raise

	def get_src(self) :
		if len(self.src) and len(self.src[-1]) : self.src.append('')                     # ensure there is \n at the end
		return (
			'\n'.join(self.src)
		,	{k for k,v in self.seen.items() if v is self.InSet}
		,	self.debug_info
		)

	have_name = {
		'LOAD_GLOBAL','STORE_GLOBAL','DELETE_GLOBAL'
	,	'LOAD_NAME'  ,'STORE_NAME'  ,'DELETE_NAME'
	}
	@staticmethod
	def get_glbs(code) :
		'recursively find func globals'
		# for global references, we need to inspect code as code.co_names contains much more
		def gather_codes(code) :                                               # gather all code objects as there may be function defs within a function
			if code in codes : return
			codes[code] = None
			for c in code.co_consts :
				if isinstance(c,types.CodeType) : gather_codes(c)
		codes = {}                                                             # use dict to retain order so order is stable
		gather_codes(code)
		glb_names = {}                                                         # use dict to retain order so order is stable
		for c in codes :
			for i in dis.Bytecode(c) :
				if i.opname in Serialize.have_name : glb_names[i.argval] = None
		return glb_names

	def val_src(self,name,val,*,force=False) :
		if not name :
			try                   : name = val.__name__
			except AttributeError : pass
		if name :
			if name in self.seen :
				if val==self.seen[name] : return
				else                    : raise f'name conflict : {name} is both {val} and {self.seen[name]}'
			self.seen[name] = val
		if isinstance(val,types.ModuleType) :
			if name==val.__name__ : self.src.append(f'import {val.__name__}'          )
			else                  : self.src.append(f'import {val.__name__} as {name}')
		elif hasattr(val,'__module__') and hasattr(val,'__qualname__') and val.__module__ not in self.by_values and not force :
			if '.' in val.__qualname__ :
				# use {name} to temporarily hold the module as it is guaranteed to be an available name
				self.src.append(f'import {val.__module__} as {name} ; {name} = {name}.{val.__qualname__}')
			else :
				if name==val.__qualname__ : self.src.append(f'from {val.__module__} import {val.__qualname__}'          )
				else                      : self.src.append(f'from {val.__module__} import {val.__qualname__} as {name}')
		elif isinstance(val,types.FunctionType) :
			self.func_src(name,val)
		elif name :
			self.src.append(f'{name} = {self.expr_src(val,force=force)}')

	def expr_src(self,val,*,force=False,call_callables=False) :
		if isinstance(val,types.ModuleType) :
			self.src.append(f'import {val.__name__}')
			return val.__name__
		sfx = ''
		if call_callables and callable(val) :
			inspect.signature(val).bind()                                                                                     # check val can be called with no argument
			sfx = '()'                                                                                                        # call val if possible and required
		if hasattr(val,'__module__') and hasattr(val,'__qualname__') and val.__module__ not in self.by_values and not force :
			self.src.append(f'import {val.__module__}')
			return f'{val.__module__}.{val.__qualname__}{sfx}'
		if isinstance(val,types.FunctionType) :
			self.func_src(val.__name__,val)
			return f'{val.__name__}{sfx}'
		kwds = { 'force':force , 'call_callables':call_callables }
		if self.has_repr(val)    : return repr(val)
		if isinstance(val,tuple) : return f"( { ' , '.join(   self.expr_src(x,**kwds)                             for x   in val        )} {',' if len(val)==1 else ''})"
		if isinstance(val,list ) : return f"[ { ' , '.join(   self.expr_src(x,**kwds)                             for x   in val        )} ]"
		if isinstance(val,set  ) : return f"{{ {' , '.join(   self.expr_src(x,**kwds)                             for x   in val        )} }}" if len(val) else "set()"
		if isinstance(val,dict ) : return f"{{ {' , '.join(f'{self.expr_src(k,**kwds)}:{self.expr_src(v,**kwds)}' for k,v in val.items())} }}"
		if isinstance(val,f_str) :
			fs = _mk_f_string(val)
			try :
				cfs = compile(fs,'','eval')
			except SyntaxError as e :
				raise SyntaxError(f'{e} : {val!r}')
			for glb_var in self.get_glbs(cfs) : self.gather_ctx(glb_var)
			return fs
		if val.__class__.__module__ not in self.by_values :
			# by default, use the broadest serializer available : pickle
			# inconvenient is that the resulting source is everything but readable
			# protocol 0 is the least unreadable, though, so use it
			val_str = pickle.dumps(val,protocol=0).decode()
			self.src.append(f'import pickle')
			return f'pickle.loads({val_str!r}.encode()){sfx}'
		raise ValueError(f'dont know how to serialize {val}')

	def gather_ctx(self,name) :
		for d in self.ctx :
			if name not in d : continue
			try    : val = d[name]
			except : self.seen[name] = self.InSet
			else   : self.val_src(name,val)
			return
		# name may be a builtins or it does not exist, in both case, do nothing and we'll have a NameError exception at runtime if name is accessed and it is not a builtin

	def get_first_line(self,name,func,first_line) :
		#
		# if this does not work, the we have to mimic inspect.Signature.__str__ and replace calls to repr by smarter self.get_src calls
		if not self.has_repr(func.__defaults__) : raise ValueError('defaults for func {func.__qualname__} are too difficult to analyze')
		#
		# if this does not work, then we have to call the tokenizer to find the correct : that splits signature from core
		if   first_line[-1]==':'                                : core = ''                                  # if line ends with :, it is a multi-line func and signature is the entire first line
		elif first_line.count(':')==1                           : core = first_line[first_line.find(':')+1:] # if there is a single :, there is not choice
		elif not func.__defaults__ and not func.__annotations__ : core = first_line[first_line.find(':')+1:] # if not default nor annotations, the first : is necessarily correct
		else                                                    : raise ValueError('core for func {func.__qualname__} is too difficult to analyze')
		#
		return f'def {name}{inspect.signature(func)} :{core}'

	def func_src(self,name,func) :
		code     = func.__code__
		module   = func.__module__
		filename = osp.abspath(code.co_filename)
		_analyze(filename)
		file_src       = srcs      [filename]
		file_end_lines = end_liness[filename]
		first_line_no1 = code.co_firstlineno                                   # first line is 1
		first_line_no0 = first_line_no1-1                                      # first line is 0
		end_line_no    = file_end_lines.get(first_line_no0)
		if first_line_no0>0 and file_src[first_line_no0-1].strip()[0:1]=='@' : raise ValueError(f'decorator not supported for {name}')
		assert end_line_no,f'{filename}:{first_line_no1} : cannot find def {name}'
		#
		if func.__globals__ not in self.ctx : self.ctx.append(func.__globals__)
		for glb_var in self.get_glbs(code) :
			self.gather_ctx(glb_var)
		#
		if self.root_dir  : filename = osp.relpath(filename,self.root_dir)
		if True           : self.src.append( self.get_first_line( name , func , file_src[first_line_no0] ) ) # first line
		if True           : self.src.extend( file_src[ first_line_no0+1 : end_line_no ]                    ) # other lines
		if self.decorator : self.src.append( f'{name} = {self.decorator}({name})'                          ) # dont synthetize a decorator as this would mix up line numbers in pdb/pudb
		self.debug_info[name] = (func.__module__,func.__qualname__,filename,first_line_no1)
