#include "tr_local.hh"

static char const * q3glsl_v = 
R"GLSL(
#version 450
	
layout(location = 0) in vec3 vert;
layout(location = 1) in vec2 uv;
uniform mat4 vertex_matrix;
uniform mat3 uv_matrix;
out vec2 f_uv;
void main() {
	f_uv = (uv_matrix * vec3(uv, 1)).xy;
	gl_Position = vertex_matrix * vec4(vert, 1);
}
)GLSL";

static char const * q3glsl_f = 
R"GLSL(
#version 450
	
in vec2 f_uv;
out vec4 color;
uniform vec4 q3color;
layout(binding = 0) uniform sampler2D tex;
void main() {
	color = texture(tex, f_uv) * q3color;
}
)GLSL";

void rend::initialize_shader() {
	
	GLint success;
	q3program = glCreateProgram();
	GLuint q3vertshad = glCreateShader(GL_VERTEX_SHADER), q3fragshad = glCreateShader(GL_FRAGMENT_SHADER);
	
	glShaderSource(q3vertshad, 1, &q3glsl_v, 0);
	glCompileShader(q3vertshad);
	glGetShaderiv(q3vertshad, GL_COMPILE_STATUS, &success);
	if (!success) {
		char * err = va_next();
		GLsizei max_length = MAX_VA_STRING;
		glGetShaderInfoLog(q3vertshad, max_length, &max_length, err);
		Com_Error(ERR_FATAL, "FATAL: vertex shader failed to compile:\n%s", err);
	}
	glShaderSource(q3fragshad, 1, &q3glsl_f, 0);
	glCompileShader(q3fragshad);
	glGetShaderiv(q3fragshad, GL_COMPILE_STATUS, &success);
	if (!success) {
		char * err = va_next();
		GLsizei max_length = MAX_VA_STRING;
		glGetShaderInfoLog(q3fragshad, max_length, &max_length, err);
		Com_Error(ERR_FATAL, "FATAL: fragment shader failed to compile:\n%s", err);
	}
	
	glAttachShader(q3program, q3vertshad);
	glAttachShader(q3program, q3fragshad);
	
	glLinkProgram(q3program);
	glGetProgramiv(q3program, GL_LINK_STATUS, &success);
	if (!success) {
		char * err = va_next();
		GLsizei max_length = MAX_VA_STRING;
		glGetProgramInfoLog(q3program, max_length, &max_length, err);
		Com_Error(ERR_FATAL, "FATAL: shader program failed to link:\n%s", err);
	}
	
	glUseProgram(q3program);
	
	q3u(q3uniform::vertex_matrix) = glGetUniformLocation(q3program, "vertex_matrix");
	q3u(q3uniform::uv_matrix) = glGetUniformLocation(q3program, "uv_matrix");
	q3u(q3uniform::q3color) = glGetUniformLocation(q3program, "q3color");
	
	glCreateSamplers(1, &q3sampler);
	glBindSampler(0, q3sampler);
	glSamplerParameteri(q3sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(q3sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	
	auto & ds = shaders.emplace_back();
	ds.in_use = true;
	ds.name = "*default";
	ds.index = 0;
	shader_lookup[ds.name] = ds.index;
	
	int num_shader_files;
	char * * shfiles = ri.FS_ListFiles("shaders", ".shader", &num_shader_files);
	for (int i = 0; i < num_shader_files; i++) {
		
		fileHandle_t f;
		char const * token, * p, * pOld;
		istring name;
		std::string shdata;
		int line;
		
		int len = ri.FS_FOpenFileRead(va("shaders/%s", shfiles[i]), &f, qfalse);
		std::string ff;
		ff.resize(len);
		ri.FS_Read(&ff[0], len, f);
		
		COM_BeginParseSession(shfiles[i]);
		p = ff.c_str();
		while (true) {
			token = COM_ParseExt(&p, qtrue);
			line = COM_GetCurrentParseLine();
			if (!token || !token[0]) break;
			if (token[0] == '#' || (token[0] == '/' && token[1] == '/')) {
				SkipRestOfLine(&p);
				continue;
			}
			name = token;
			pOld = p;
			token = COM_ParseExt(&p, qtrue);
			if (token[0] != '{' || token[1] != '\0') {
				Com_Printf(S_COLOR_RED "ERROR: Shader (\"%s\") without brace section on line %i, aborting.\n", name.c_str(), line);
				break;
			}
			if (!SkipBracedSection(&p, 1)) {
				Com_Printf(S_COLOR_RED "ERROR: Shader (\"%s\") seems to be missing closing brace, aborting.\n", name.c_str());
				break;
			}
			while (true) {
				switch (pOld [0]) {
					default:
						pOld++;
						assert(pOld < p);
						continue;
					case '{':
						break;
				}
				break;
			}
			shader_source_lookup[name] = {pOld, p};
		}
		ri.FS_FCloseFile(f);
	}
	ri.FS_FreeFileList(shfiles);
}

void rend::destruct_shader() noexcept {
	if (q3program) glDeleteProgram(q3program);
	if (q3sampler) glDeleteSamplers(1, &q3sampler);
}

static bool parse_shader(q3shader & shad, char const * src, bool mipmaps);

qhandle_t rend::register_shader(char const * name, bool mipmaps) {
	
	auto m = shader_lookup.find(name);
	if (m != shader_lookup.end()) return m->second;
	
	qhandle_t handle = -1;
	
	for (size_t i = 0; i < shaders.size(); i++) {
		if (!shaders[i].in_use) {
			handle = i;
		}
	}
	
	if (handle == -1) {
		handle = shaders.size();
		shaders.emplace_back();
	}
	
	q3shader & shad = shaders[handle];
	shad.in_use = true;
	shad.index = handle;
	shad.name = name;
	
	auto src = shader_source_lookup.find(name);
	if (src == shader_source_lookup.end()) {
		
		GLuint tex = r->register_texture(name);
		if (!tex) {
			Com_Printf(S_COLOR_RED "ERROR: Could not find image for shader '%s'!\n", name);
			shad = {};
			shader_lookup[name] = 0;
			return 0;
		}
		
		shad.stages.emplace_back();
		shad.stages[0].diffuse = tex;
		shad.stages[0].blend_src = GL_SRC_ALPHA;
		shad.stages[0].blend_dst = GL_ONE_MINUS_SRC_ALPHA;
		shader_lookup[name] = handle;
		Com_Printf("Image Shader: %s\n", name);
		return handle;
	}
	
	if (!parse_shader(shad, src->second.c_str(), mipmaps)) {
		Com_Printf(S_COLOR_RED "ERROR: Could not parse shader '%s'!\n", name);
		shad = {};
		shader_lookup[name] = 0;
		return 0;
	}
	
	shader_lookup[name] = handle;
	Com_Printf("Q3 Shader: %s\n", name);
	return handle;
}

static float gen_func_do(q3stage::gen_func func, float in, float base, float amplitude, float phase, float frequency) {
	switch (func) {
		default: 
			break;
		case q3stage::gen_func::sine:
			return base + sin((in + phase) * frequency * math::pi<float> * 2) * amplitude;
	}
	return 0;
}

void rend::shader_set_vp(rm4_t const & vp) {
	this->vp = vp;
}

static float	s_flipMatrix[16] = {
	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	0, 0, -1, 0,
	-1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};

static void myGlMultMatrix( const float *a, const float *b, float *out ) {
	int		i, j;

	for ( i = 0 ; i < 4 ; i++ ) {
		for ( j = 0 ; j < 4 ; j++ ) {
			out[ i * 4 + j ] =
				a [ i * 4 + 0 ] * b [ 0 * 4 + j ]
				+ a [ i * 4 + 1 ] * b [ 1 * 4 + j ]
				+ a [ i * 4 + 2 ] * b [ 2 * 4 + j ]
				+ a [ i * 4 + 3 ] * b [ 3 * 4 + j ];
		}
	}
}

void rend::shader_set_m(rm4_t const & m, bool flip) {
	if (false) {
		rm4_t mvp = m * vp;
		rm4_t adj;
		myGlMultMatrix(mvp, s_flipMatrix, &adj.data[0][0]);
		glUniformMatrix4fv(q3u(q3uniform::vertex_matrix), 1, GL_FALSE, adj);
	} else {
		glUniformMatrix4fv(q3u(q3uniform::vertex_matrix), 1, GL_FALSE, m * vp);
	}
}

void rend::shader_setup_stage(q3stage const & stage, rm3_t const & uvm, float time) {
	glBindTextureUnit(0, stage.diffuse);
	glBlendFunc(stage.blend_src, stage.blend_dst);
	
	rv4_t q3color {1, 1, 1, 1};
	switch (stage.gen_rgb) {
		case q3stage::gen_type::vertex:
		case q3stage::gen_type::none:
			q3color = color_2d;
			break;
		case q3stage::gen_type::constant:
			q3color = stage.color;
			break;
		case q3stage::gen_type::wave:
			float mult = gen_func_do(stage.wave.rgb.func, time, stage.wave.rgb.base, stage.wave.rgb.amplitude, stage.wave.rgb.phase, stage.wave.rgb.frequency);
			q3color = stage.color * mult;
			break;
	}
	
	switch (stage.gen_alpha) {
		case q3stage::gen_type::vertex:
		case q3stage::gen_type::none:
			q3color[3] = color_2d[3];
			break;
		case q3stage::gen_type::constant:
			q3color[3] = stage.color[3];
			break;
		case q3stage::gen_type::wave:
			float mult = gen_func_do(stage.wave.rgb.func, time, stage.wave.rgb.base, stage.wave.rgb.amplitude, stage.wave.rgb.phase, stage.wave.rgb.frequency);
			q3color[3] = stage.color[3] * mult;
			break;
	}

	rm3_t uvm2 = uvm;
	for (q3stage::texmod const & t : stage.texmods) {
		switch (t.type) {
			case q3stage::texmod::tctype::scale:
				uvm2 *= rm3_t::scale(t.scale_data.scale[0], t.scale_data.scale[1]);
				break;
			case q3stage::texmod::tctype::scroll:
				uvm2 *= rm3_t::translate(t.scroll_data.scroll[0] * time, t.scroll_data.scroll[1] * time);
				break;
			case q3stage::texmod::tctype::transform:
				uvm2 *= t.transform_data.trans;
				break;
			case q3stage::texmod::tctype::rotate:
				uvm2 *= rm3_t::translate(-0.5, -0.5);
				uvm2 *= rm3_t::rotate(t.rotate_data.rot * time);
				uvm2 *= rm3_t::translate(0.5, 0.5);
				break;
			default:
				ri.Printf(PRINT_WARNING, "WARNING: Unknown texmod type (%i) in shader stage!\n", static_cast<int>(t.type));
				break;
		}
	}
	
	glUniform4fv(q3u(q3uniform::q3color), 1, q3color);
	glUniformMatrix3fv(q3u(q3uniform::uv_matrix), 1, GL_FALSE, uvm2);
	
	if (stage.clamp) {
		glSamplerParameteri(q3sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glSamplerParameteri(q3sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	} else {
		glSamplerParameteri(q3sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glSamplerParameteri(q3sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
	}
}

static std::unordered_map<std::string, GLenum> blendfunc_map = {
	{"GL_ONE",						GL_ONE},
	{"GL_ZERO",						GL_ZERO},
	{"GL_SRC_COLOR",				GL_SRC_COLOR},
	{"GL_DST_COLOR",				GL_DST_COLOR},
	{"GL_SRC_ALPHA",				GL_SRC_ALPHA},
	{"GL_DST_ALPHA",				GL_DST_ALPHA},
	{"GL_ONE_MINUS_SRC_COLOR",		GL_ONE_MINUS_SRC_COLOR},
	{"GL_ONE_MINUS_DST_COLOR",		GL_ONE_MINUS_DST_COLOR},
	{"GL_ONE_MINUS_SRC_ALPHA",		GL_ONE_MINUS_SRC_ALPHA},
	{"GL_ONE_MINUS_DST_ALPHA",		GL_ONE_MINUS_DST_ALPHA},
};

static GLenum blendfunc_str2enum(char const * str) {
	if (!str) return GL_ONE;
	auto const & i = blendfunc_map.find(str);
	if (i == blendfunc_map.end()) {
		Com_Printf(S_COLOR_RED "ERROR: invalid blendfunc requested: (\"%s\"), defaulting to GL_ONE.\n", str);
		return GL_ONE;
	}
	else return i->second;
}

static std::unordered_map<std::string, q3stage::gen_func> genfunc_map = {
	{"sin",					q3stage::gen_func::sine},
	{"square",				q3stage::gen_func::square},
	{"triangle",			q3stage::gen_func::triangle},
	{"sawtooth",			q3stage::gen_func::sawtooth},
	{"inverse_sawtooth",	q3stage::gen_func::inverse_sawtooth},
	{"noise",				q3stage::gen_func::noise},
	{"random",				q3stage::gen_func::random},
};

static q3stage::gen_func genfunc_str2enum(char const * str) {
	if (!str) return q3stage::gen_func::random;
	auto const & i = genfunc_map.find(str);
	if (i == genfunc_map.end()) {
		Com_Printf(S_COLOR_RED "ERROR: invalid genfunc requested: (\"%s\"), defaulting to random.\n", str);
		return q3stage::gen_func::random;
	}
	else return i->second;
}

static void parse_texmod(char const * name, q3stage & stg, char const * p) {
	
	char const * token = COM_ParseExt( &p, qfalse );
	
	if (!Q_stricmp( token, "turb")) {
		
		stg.texmods.emplace_back(q3stage::texmod::tctype::turb);
		auto & tc = stg.texmods.back();
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod turb parm \"base\" in shader (\"%s\")\n", name);
			return;
		}
		tc.turb_data.base = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod turb parm \"amplitude\" in shader (\"%s\")\n", name);
			return;
		}
		tc.turb_data.amplitude = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod turb parm \"phase\" in shader (\"%s\")\n", name);
			return;
		}
		tc.turb_data.phase = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod turb parm \"frequency\" in shader (\"%s\")\n", name);
			return;
		}
		tc.turb_data.frequency = strtof(token, nullptr);
		
	} else if (!Q_stricmp( token, "scale")) {
		
		stg.texmods.emplace_back(q3stage::texmod::tctype::scale);
		auto & tc = stg.texmods.back();
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod scale parm \"x\" in shader (\"%s\")\n", name);
			return;
		}
		tc.scale_data.scale[0] = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod scale parm \"y\" in shader (\"%s\")\n", name);
			return;
		}
		tc.scale_data.scale[1] = strtof(token, nullptr);
		
	} else if (!Q_stricmp( token, "scroll")) {
		
		stg.texmods.emplace_back(q3stage::texmod::tctype::scroll);
		auto & tc = stg.texmods.back();
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod scroll parm \"x\" in shader (\"%s\")\n", name);
			return;
		}
		tc.scroll_data.scroll[0] = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod scroll parm \"y\" in shader (\"%s\")\n", name);
			return;
		}
		tc.scroll_data.scroll[1] = strtof(token, nullptr);
		
	} else if (!Q_stricmp( token, "stretch")) {
		
		stg.texmods.emplace_back(q3stage::texmod::tctype::stretch);
		auto & tc = stg.texmods.back();
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod stretch parm \"genfunc\" in shader (\"%s\")\n", name);
			return;
		}
		tc.stretch_data.gfunc = genfunc_str2enum(token);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod stretch parm \"base\" in shader (\"%s\")\n", name);
			return;
		}
		tc.stretch_data.base = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod stretch parm \"amplitude\" in shader (\"%s\")\n", name);
			return;
		}
		tc.stretch_data.amplitude = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod stretch parm \"phase\" in shader (\"%s\")\n", name);
			return;
		}
		tc.stretch_data.phase= strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod stretch parm \"frequency\" in shader (\"%s\")\n", name);
			return;
		}
		tc.stretch_data.frequency = strtof(token, nullptr);
		
	} else if (!Q_stricmp( token, "transform")) {

		stg.texmods.emplace_back(q3stage::texmod::tctype::transform);
		auto & tc = stg.texmods.back();
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod transform parm \"[0][0]\" in shader (\"%s\")\n", name);
			return;
		}
		tc.transform_data.trans[0][0] = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod transform parm \"[0][1]\" in shader (\"%s\")\n", name);
			return;
		}
		tc.transform_data.trans[0][1] = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod transform parm \"[1][0]\" in shader (\"%s\")\n", name);
			return;
		}
		tc.transform_data.trans[1][0] = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod transform parm \"[1][1]\" in shader (\"%s\")\n", name);
			return;
		}
		tc.transform_data.trans[1][1] = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod transform parm \"[2][0]\" in shader (\"%s\")\n", name);
			return;
		}
		tc.transform_data.trans[2][0] = strtof(token, nullptr);
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod transform parm \"[2][1]\" in shader (\"%s\")\n", name);
			return;
		}
		tc.transform_data.trans[2][1] = strtof(token, nullptr);
		
	} else if (!Q_stricmp( token, "rotate")) {

		stg.texmods.emplace_back(q3stage::texmod::tctype::rotate);
		auto & tc = stg.texmods.back();
		token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: missing tcMod rotate parm \"value\" in shader (\"%s\")\n", name);
			return;
		}
		tc.rotate_data.rot = strtof(token, nullptr);
		tc.rotate_data.rot = math::deg2rad<float>(tc.rotate_data.rot);
	} else {
		Com_Printf(S_COLOR_RED "ERROR: unknown tcMod parameter \"%s\" in shader (\"%s\")\n", token, name);
		return;
	}
}

static qboolean parse_vector ( const char **text, int count, float *v ) {
	char	*token;
	int		i;

	// FIXME: spaces are currently required after parens, should change parseext...
	token = COM_ParseExt( text, qfalse );
 	if ( strcmp( token, "(" ) ) {
		//ri.Printf( PRINT_ALL, S_COLOR_YELLOW  "ERROR: missing parenthesis in shader '%s'\n", shader.name );
		return qfalse;
	}

	for ( i = 0 ; i < count ; i++ ) {
		token = COM_ParseExt( text, qfalse );
		if ( !token[0] ) {
			//ri.Printf( PRINT_ALL, S_COLOR_YELLOW  "ERROR: missing vector element in shader '%s'\n", shader.name );
			return qfalse;
		}
		v[i] = atof( token );
	}

	token = COM_ParseExt( text, qfalse );
	if ( strcmp( token, ")" ) ) {
		//ri.Printf( PRINT_ALL, S_COLOR_YELLOW  "ERROR: missing parenthesis in shader '%s'\n", shader.name );
		return qfalse;
	}

	return qtrue;
}

static bool parse_stage(char const * name, q3stage & stg, char const * & p, bool mipmaps) {
	
	char const * token;
	
	while(true) {
		
		token = COM_ParseExt(&p, qtrue);
		
		if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: shader stage for (\"%s\") ends abruptly.\n", name);
			return false;
		}
		
		else if (token[0] == '}') break;
		
		else if (!Q_stricmp("map", token)) {
			token = COM_ParseExt(&p, qfalse);
			stg.diffuse = r->register_texture(token, mipmaps);
			if (!stg.diffuse) {
				Com_Printf(S_COLOR_RED "ERROR: shader stage for (\"%s\") has invalid map (\"%s\"), could not find this image.\n", name, token);
				return false;
			}
		}
		
		else if (!Q_stricmp("clampmap", token)) {
			token = COM_ParseExt(&p, qfalse);
			stg.diffuse = r->register_texture(token, mipmaps);
			if (!stg.diffuse) {
				Com_Printf(S_COLOR_RED "ERROR: shader stage for (\"%s\") has invalid clampmap (\"%s\"), could not find this image.\n", name, token);
				return false;
			}
			stg.clamp = true;
		}
		
		else if (!Q_stricmp("blendFunc", token)) {
			token = COM_ParseExt(&p, qfalse);
			stg.blend_src = blendfunc_str2enum(token);
			token = COM_ParseExt(&p, qfalse);
			stg.blend_dst = blendfunc_str2enum(token);
		}
		
		else if (!Q_stricmp("rgbGen", token)) {
			token = COM_ParseExt(&p, qfalse);
			if (!Q_stricmp("const", token)) {
				vec3_t color;
				parse_vector(&p, 3, color);
				stg.gen_rgb = q3stage::gen_type::constant;
				stg.color[0] = color[0];
				stg.color[1] = color[1];
				stg.color[2] = color[2];
			} else if (!Q_stricmp("identity", token)) {
				stg.gen_rgb = q3stage::gen_type::constant;
				stg.color[0] = 1;
				stg.color[1] = 1;
				stg.color[2] = 1;
			} else if (!Q_stricmp("vertex", token)) {
				stg.gen_rgb = q3stage::gen_type::vertex;
			} else if (!Q_stricmp("wave", token)) {
				stg.gen_rgb = q3stage::gen_type::wave;
				token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
					Com_Printf(S_COLOR_YELLOW "WARNING: missing tcMod stretch parm \"genfunc\" in shader (\"%s\")\n", name);
					SkipRestOfLine(&p);
					continue;
				}
				stg.wave.rgb.func = genfunc_str2enum(token);
				token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
					Com_Printf(S_COLOR_YELLOW "WARNING: missing tcMod stretch parm \"base\" in shader (\"%s\")\n", name);
					SkipRestOfLine(&p);
					continue;
				}
				stg.wave.rgb.base = strtof(token, nullptr);
				token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
					Com_Printf(S_COLOR_YELLOW "WARNING: missing tcMod stretch parm \"amplitude\" in shader (\"%s\")\n", name);
					SkipRestOfLine(&p);
					continue;
				}
				stg.wave.rgb.amplitude = strtof(token, nullptr);
				token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
					Com_Printf(S_COLOR_YELLOW "WARNING: missing tcMod stretch parm \"phase\" in shader (\"%s\")\n", name);
					SkipRestOfLine(&p);
					continue;
				}
				stg.wave.rgb.phase= strtof(token, nullptr);
				token = COM_ParseExt( &p, qfalse ); if (!token[0]) {
					Com_Printf(S_COLOR_YELLOW "WARNING: missing tcMod stretch parm \"frequency\" in shader (\"%s\")\n", name);
					SkipRestOfLine(&p);
					continue;
				}
				stg.wave.rgb.frequency = strtof(token, nullptr);
			} else {
				Com_Printf(S_COLOR_YELLOW "WARNING: shader stage for (\"%s\") has unknown/invalid rgbGen (\"%s\").\n", name, token);
				SkipRestOfLine(&p);
				continue;
			}
		}
		
		else if (!Q_stricmp("alphaGen", token)) {
			token = COM_ParseExt(&p, qfalse);
			if (!Q_stricmp("const", token)) {
				token = COM_ParseExt(&p, qfalse);
				stg.gen_alpha = q3stage::gen_type::constant;
				stg.color[3] = strtof(token, nullptr);
			} else if (!Q_stricmp("identity", token)) {
				stg.gen_alpha = q3stage::gen_type::constant;
				stg.color[3] = 1;
			} else if (!Q_stricmp("vertex", token)) {
				stg.gen_alpha = q3stage::gen_type::vertex;
			} else {
				Com_Printf(S_COLOR_YELLOW "WARNING: shader stage for (\"%s\") has unknown/invalid alphaGen (\"%s\").\n", name, token);
				SkipRestOfLine(&p);
				continue;
			}
		}
		
		else if (!Q_stricmp(token, "tcMod")) {
			char buffer [1024] = "";
			while (true) {
				token = COM_ParseExt(&p, qfalse);
				if (!token[0]) break;
				Q_strcat( buffer, sizeof( buffer ), token );
				Q_strcat( buffer, sizeof( buffer ), " " );
			}
			parse_texmod(name, stg, buffer);
			
		} else if (!Q_stricmp(token, "glow")) {
			//TODO
		} else {
			
			Com_Printf(S_COLOR_YELLOW "WARNING: shader stage for (\"%s\") has unknown/invalid key (\"%s\").\n", name, token);
			SkipRestOfLine(&p);
			continue;
		}
	}
	
	return true;
}

static bool parse_shader(q3shader & shad, char const * src, bool mipmaps) {
	char const * token, * p = src;
	COM_BeginParseSession("shader");
	
	token = COM_ParseExt(&p, qtrue);
	if (token[0] != '{') {
		Com_Printf(S_COLOR_RED "ERROR: shader (\"%s\") missing definition.\n", shad.name.c_str());
		return false;
	}
	
	while (true) {
		
		token = COM_ParseExt(&p, qtrue);
		
		if (!token[0]) {
			Com_Printf(S_COLOR_RED "ERROR: shader (\"%s\") ends abruptly.\n", shad.name.c_str());
			return false;
		} 
		else if (token[0] == '}') break;
		else if (token[0] == '{') {
			q3stage stg;
			if (!parse_stage(shad.name.c_str(), stg, p, mipmaps)) {
				Com_Printf(S_COLOR_RED "ERROR: shader stage for (\"%s\") failed to parse.\n", shad.name.c_str());
				return false;
			}
			shad.stages.push_back(stg);
		} else if (!Q_stricmp(token, "nopicmip")) {
			SkipRestOfLine(&p);
			// TODO
		} else if (!Q_stricmp(token, "nomipmaps")) {
			SkipRestOfLine(&p);
			// TODO
		} else if (!Q_stricmp(token, "qer_editorimage")) {
			SkipRestOfLine(&p);
		} else {
			Com_Printf(S_COLOR_YELLOW "WARNING: shader (\"%s\") has unknown/invalid key (\"%s\").\n", shad.name.c_str(), token);
			SkipRestOfLine(&p);
		}
		
	}
	
	return true;
}