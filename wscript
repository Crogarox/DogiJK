#!/bin/python

from waflib import *
import os, sys

top = os.getcwd()
out = 'build'
	
g_comflags = ['-pthread', '-w']
g_cflags = ['-std=c11'] + g_comflags
g_cxxflags = ['-std=c++14'] + g_comflags

def btype_cflags(ctx):
	return {
		'DEBUG'   : g_cflags + ['-Og', '-ggdb3', '-march=core2', '-mtune=native'],
		'NATIVE'  : g_cflags + ['-Ofast', '-march=native', '-mtune=native'],
		'RELEASE' : g_cflags + ['-O3', '-march=core2', '-mtune=generic'],
	}.get(ctx.env.BUILD_TYPE, g_cflags)
def btype_cxxflags(ctx):
	return {
		'DEBUG'   : g_cxxflags + ['-Og', '-ggdb3', '-march=core2', '-mtune=native'],
		'NATIVE'  : g_cxxflags + ['-Ofast', '-march=native', '-mtune=native'],
		'RELEASE' : g_cxxflags + ['-O3', '-march=core2', '-mtune=generic'],
	}.get(ctx.env.BUILD_TYPE, g_cxxflags)

def options(opt):
	opt.load('gcc')
	opt.load('g++')
	
	opt.add_option('--build_type', dest='build_type', type='string', default='RELEASE', action='store', help='DEBUG, NATIVE, RELEASE')
	opt.add_option('--bash_location', dest='bash_location', type='string', default='bash', action='store', help='Location of bash when compiling on Windows. Optional.')
	opt.add_option('--no_server', dest='bldsv', default=True, action='store_false', help='True/False')
	opt.add_option('--no_client', dest='bldcl', default=True, action='store_false', help='True/False')

def configure(ctx):
	
	ctx.env.BUILD_SERVER = ctx.options.bldsv
	ctx.env.BUILD_CLIENT = ctx.options.bldcl
	
	ctx.load('gcc')
	ctx.load('g++')
	
	ctx.check(features='c cprogram', lib='z', uselib_store='ZLIB')
	ctx.check(features='c cprogram', lib='dl', uselib_store='DL')
	
	if ctx.env.BUILD_CLIENT:
		ctx.check(features='c cprogram', lib='jpeg', uselib_store='JPEG')
		ctx.check(features='c cprogram', lib='png', uselib_store='PNG')
	ctx.check(features='c cprogram', lib='pthread', uselib_store='PTHREAD')
	ctx.check(features='c cprogram', lib='GL', uselib_store='GL')
	ctx.check_cfg(path='sdl2-config', args='--cflags --libs', package='', uselib_store='SDL')
	
	btup = ctx.options.build_type.upper()
	if btup in ['DEBUG', 'NATIVE', 'RELEASE']:
		Logs.pprint('PINK', 'Setting up environment for known build type: ' + btup)
		ctx.env.BUILD_TYPE = btup
		ctx.env.CFLAGS = btype_cflags(ctx)
		ctx.env.CXXFLAGS = btype_cxxflags(ctx)
		Logs.pprint('PINK', 'CFLAGS: ' + ' '.join(ctx.env.CFLAGS))
		Logs.pprint('PINK', 'CXXFLAGS: ' + ' '.join(ctx.env.CXXFLAGS))
		if btup == 'DEBUG':
			ctx.define('_DEBUG', 1)
		else:
			ctx.define('NDEBUG', 1)
		ctx.define('ARCH_STRING', 'x86_64')
	else:
		Logs.error('UNKNOWN BUILD TYPE: ' + btup)

def build(bld):
	
	### SHARED FILES ###
	
	shared_files = []
	shared_files += bld.path.ant_glob('src/qcommon/q_color.c')
	shared_files += bld.path.ant_glob('src/qcommon/q_math.c')
	shared_files += bld.path.ant_glob('src/qcommon/q_string.c')
	
	### MINIZIP ###
	
	minizip_files = bld.path.ant_glob('src/minizip/*.c')
	minizip = bld (
		features = 'c cstlib',
		target = 'minizip',
		includes = 'src/minizip/include/minizip',
		source = minizip_files,
	)
	
	### BOTLIB ###
	
	botlib_files = bld.path.ant_glob('src/botlib/*.cpp')
	botlib_files += bld.path.ant_glob('src/qcommon/q_shared.c')
	
	botlib = bld (
		features = 'cxx cxxstlib',
		target = 'botlib',
		includes = ['src'],
		source = shared_files + botlib_files,
		defines = ['BOTLIB'],
	)
	
	### SERVER CLIENT SHARED ###
	
	clsv_files = []
	clsv_files += bld.path.ant_glob('src/qcommon/*.cpp')
	clsv_files += bld.path.ant_glob('src/icarus/*.cpp')
	clsv_files += bld.path.ant_glob('src/server/*.cpp')
	clsv_files += bld.path.ant_glob('src/server/NPCNav/*.cpp')
	clsv_files += bld.path.ant_glob('src/mp3code/*.c')
	clsv_files += bld.path.ant_glob('src/sys/snapvector.cpp')
	clsv_files += bld.path.ant_glob('src/sys/sys_main.cpp')
	clsv_files += bld.path.ant_glob('src/sys/sys_event.cpp')
	clsv_files += bld.path.ant_glob('src/sys/sys_log.cpp')
	clsv_files += bld.path.ant_glob('src/sys/con_log.cpp')
	clsv_files += bld.path.ant_glob('src/sys/sys_unix.cpp')
	clsv_files += bld.path.ant_glob('src/sys/con_tty.cpp')
	
	### CLIENT ###
	
	if bld.env.BUILD_CLIENT:
	
		client_files = []
		client_files += bld.path.ant_glob('src/client/*.cpp')
		client_files += bld.path.ant_glob('src/sdl/sdl_window.cpp')
		client_files += bld.path.ant_glob('src/sdl/sdl_input.cpp')
		client_files += bld.path.ant_glob('src/sdl/sdl_sound.cpp')
		
		client = bld (
			features = 'cxx cxxprogram',
			target = 'parajk',
			includes = ['src', '/usr/include/tirpc'],
			source = shared_files + clsv_files + client_files,
			uselib = ['SDL', 'ZLIB', 'DL', 'PTHREAD'],
			use = ['minizip', 'botlib'],
			install_path = os.path.join(top, 'install')
		)
		
	# SERVER
	
	if bld.env.BUILD_SERVER:
	
		server_files = bld.path.ant_glob('src/rd-dedicated/*.cpp')
		server_files += bld.path.ant_glob('src/null/*.cpp')
		server_files += bld.path.ant_glob('src/ghoul2/*.cpp')

		server = bld (
			features = 'cxx cxxprogram',
			target = 'parajkded',
			includes = ['src', '/usr/include/tirpc'],
			source = shared_files + clsv_files + server_files,
			defines = ['_CONSOLE', 'DEDICATED'],
			uselib = ['ZLIB', 'DL', 'PTHREAD'],
			use = ['minizip', 'botlib'],
			install_path = os.path.join(top, 'install')
		)
		
	### GAME/CGAME/UI ###
	
	gcgui_files = bld.path.ant_glob('src/qcommon/*.c')
	
	# GAME
	
	game_files = bld.path.ant_glob('src/game/*.c')
	
	game = bld (
		features = 'c cshlib',
		target = 'jampgame',
		includes = ['src'],
		source = gcgui_files + game_files,
		uselib = ['PTHREAD'],
		defines = ['_GAME'],
		install_path = os.path.join(top, 'install', 'base')
	)
	
	game.env.cshlib_PATTERN = '%sx86_64.so'
	
	

	if bld.env.BUILD_CLIENT:
		# CGAME
		
		cgame_files = bld.path.ant_glob('src/cgame/*.c')
		cgame_files += bld.path.ant_glob('src/game/bg_*.c')
		cgame_files += bld.path.ant_glob('src/game/AnimalNPC.c')
		cgame_files += bld.path.ant_glob('src/game/FighterNPC.c')
		cgame_files += bld.path.ant_glob('src/game/SpeederNPC.c')
		cgame_files += bld.path.ant_glob('src/game/WalkerNPC.c')
		cgame_files += bld.path.ant_glob('src/ui/ui_shared.c')
		
		cgame = bld (
			features = 'c cshlib',
			target = 'cgame',
			includes = ['src'],
			source = gcgui_files + cgame_files,
			uselib = ['PTHREAD'],
			defines = ['_CGAME'],
			install_path = os.path.join(top, 'install', 'base')
		)
		
		cgame.env.cshlib_PATTERN = '%sx86_64.so'
		
		# UI
	
		ui_files = bld.path.ant_glob('src/ui/*.c')
		ui_files += bld.path.ant_glob('src/game/bg_misc.c')
		ui_files += bld.path.ant_glob('src/game/bg_saberLoad.c')
		ui_files += bld.path.ant_glob('src/game/bg_saga.c')
		ui_files += bld.path.ant_glob('src/game/bg_vehicleLoad.c')
		ui_files += bld.path.ant_glob('src/game/bg_weapons.c')
		
		ui = bld (
			features = 'c cshlib',
			target = 'ui',
			includes = ['src'],
			source = gcgui_files + ui_files,
			uselib = ['PTHREAD'],
			defines = ['UI_BUILD'],
			install_path = os.path.join(top, 'install', 'base')
		)
		
		ui.env.cshlib_PATTERN = '%sx86_64.so'
		
	### RD-VANILLA ###
	
		rdvan_files = bld.path.ant_glob('src/rd-vanilla/*.cpp')
		#rdvan_files += bld.path.ant_glob('src/rd-vanilla/glad.c')
		rdvan_files += bld.path.ant_glob('src/rd-common/*.cpp')
		rdvan_files += bld.path.ant_glob('src/ghoul2/*.cpp')
		rdvan_files += bld.path.ant_glob('src/qcommon/matcomp.cpp')
		rdvan_files += bld.path.ant_glob('src/qcommon/q_shared.cpp')
		
		rdvan = bld (
			features = 'c cxx cshlib cxxshlib',
			target = 'rd-vanilla',
			includes = ['src', 'src/rd-vanilla'],
			source = shared_files + rdvan_files,
			uselib = ['JPEG', 'PNG', 'GL', 'PTHREAD'],
			install_path = os.path.join(top, 'install')
		)
		
		rdvan.env.cxxshlib_PATTERN = '%s_x86_64.so'
		
def clean(ctx):
	pass