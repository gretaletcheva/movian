/*
 *  libglw, OpenGL interface
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "glw.h"
#include "glw_renderer.h"
#include "fileaccess/fileaccess.h"

#define SHADERPATH "bundle://src/ui/glw/opengl/"


// #define DEBUG_SHADERS

static const float identitymtx[16] = {
  1,0,0,0,
  0,1,0,0,
  0,0,1,0,
  0,0,0,1};
  
/**
 * return 1 if the extension is found, otherwise 0
 */
static int
check_gl_ext(const uint8_t *s, const char *func)
{
  int l = strlen(func);
  char *v;
  int x;

  v = strstr((const char *)s, func);
  x = v != NULL && v[l] < 33;

  TRACE(TRACE_DEBUG, "OpenGL", "Feature \"%s\" %savailable",
	func, x ? "" : "not ");
  return x;
}



const static float projection[16] = {
  2.414213,0.000000,0.000000,0.000000,
  0.000000,2.414213,0.000000,0.000000,
  0.000000,0.000000,1.033898,-1.000000,
  0.000000,0.000000,2.033898,0.000000
};


/**
 *
 */
static void
hw_set_clip_conf(struct glw_rctx *rc, int which, const Vec4 v)
{
  double plane[4];
    
  plane[0] = glw_vec4_extract(v, 0);
  plane[1] = glw_vec4_extract(v, 1);
  plane[2] = glw_vec4_extract(v, 2);
  plane[3] = glw_vec4_extract(v, 3);

  glLoadMatrixf(glw_mtx_get(rc->rc_mtx));

  glClipPlane(GL_CLIP_PLANE0 + which, plane);
  glEnable(GL_CLIP_PLANE0 + which);
}

/**
 *
 */
static void
hw_clr_clip_conf(struct glw_rctx *rc, int which)
{
  glDisable(GL_CLIP_PLANE0 + which);
}


/**
 *
 */
void
glw_wirebox(glw_root_t *gr, glw_rctx_t *rc)
{
#if CONFIG_GLW_BACKEND_OPENGL
  glw_backend_root_t *gbr = &gr->gr_be;
  glw_load_program(gbr, gbr->gbr_renderer_flat);
  glLoadMatrixf(glw_mtx_get(rc->rc_mtx));
  glw_program_set_uniform_color(gbr, 1,1,1,1);
  glDisable(GL_TEXTURE_2D);
  glBegin(GL_LINE_LOOP);
  glColor4f(1,1,1,1);
  glVertex3f(-1.0, -1.0, 0.0);
  glVertex3f( 1.0, -1.0, 0.0);
  glVertex3f( 1.0,  1.0, 0.0);
  glVertex3f(-1.0,  1.0, 0.0);
  glEnd();
  glEnable(GL_TEXTURE_2D);
#endif
}


/**
 *
 */
void
glw_wirecube(glw_root_t *gr, glw_rctx_t *rc)
{
#if CONFIG_GLW_BACKEND_OPENGL
  glw_backend_root_t *gbr = &gr->gr_be;

  glw_load_program(gbr, gbr->gbr_renderer_flat);
  glLoadMatrixf(glw_mtx_get(rc->rc_mtx));
  glw_program_set_uniform_color(gbr, 1,1,1,1);

  glBegin(GL_LINE_LOOP);
  glVertex3f(-1.0, -1.0, -1.0);
  glVertex3f( 1.0, -1.0, -1.0);
  glVertex3f( 1.0,  1.0, -1.0);
  glVertex3f(-1.0,  1.0, -1.0);
  glEnd();

  glBegin(GL_LINE_LOOP);
  glVertex3f(-1.0, -1.0,  1.0);
  glVertex3f( 1.0, -1.0,  1.0);
  glVertex3f( 1.0,  1.0,  1.0);
  glVertex3f(-1.0,  1.0,  1.0);
  glEnd();

  glBegin(GL_LINE_LOOP);
  glVertex3f(-1.0, -1.0,  1.0);
  glVertex3f(-1.0, -1.0, -1.0);
  glVertex3f(-1.0,  1.0, -1.0);
  glVertex3f(-1.0,  1.0,  1.0);
  glEnd();

  glBegin(GL_LINE_LOOP);
  glVertex3f( 1.0, -1.0,  1.0);
  glVertex3f( 1.0, -1.0, -1.0);
  glVertex3f( 1.0,  1.0, -1.0);
  glVertex3f( 1.0,  1.0,  1.0);
  glEnd();

  glBegin(GL_LINE_LOOP);
  glVertex3f( 1.0, -1.0,  1.0);
  glVertex3f( 1.0, -1.0, -1.0);
  glVertex3f(-1.0, -1.0, -1.0);
  glVertex3f(-1.0, -1.0,  1.0);
  glEnd();

  glBegin(GL_LINE_LOOP);
  glVertex3f( 1.0,  1.0,  1.0);
  glVertex3f( 1.0,  1.0, -1.0);
  glVertex3f(-1.0,  1.0, -1.0);
  glVertex3f(-1.0,  1.0,  1.0);
  glEnd();
#endif
}


/** 
 * Render method using OpenGL fixed function pipeline
 */
static void
ff_render(struct glw_root *gr,
	  Mtx m,
	  struct glw_backend_texture *tex,
	  const struct glw_rgb *rgb_mul,
	  const struct glw_rgb *rgb_off,
	  float alpha, float blur,
	  const float *vertices,
	  int num_vertices,
	  const uint16_t *indices,
	  int num_triangles,
	  int flags)
{
  glw_backend_root_t *gbr = &gr->gr_be;
  float r,g,b;

  switch(gbr->be_blendmode) {
  case GLW_BLEND_NORMAL:
    r = rgb_mul->r;
    g = rgb_mul->g;
    b = rgb_mul->b;
    break;
  case GLW_BLEND_ADDITIVE:
    r = rgb_mul->r * alpha;
    g = rgb_mul->g * alpha;
    b = rgb_mul->b * alpha;
    break;
  default:
    return;
  }


  glLoadMatrixf(glw_mtx_get(m) ?: identitymtx);

  glVertexPointer(3, GL_FLOAT, sizeof(float) * VERTEX_SIZE, vertices);

  if(flags & GLW_RENDER_COLOR_ATTRIBUTES) {
    int i;

    if(num_vertices > gr->gr_vtmp_capacity) {
      gr->gr_vtmp_capacity = num_vertices;
      gr->gr_vtmp_buffer = realloc(gr->gr_vtmp_buffer, sizeof(float) *
				   VERTEX_SIZE * gr->gr_vtmp_capacity);
    }
    for(i = 0; i < num_vertices; i++) {
      gr->gr_vtmp_buffer[i * VERTEX_SIZE + 0] =
	vertices[i * VERTEX_SIZE + 5] * r;
      gr->gr_vtmp_buffer[i * VERTEX_SIZE + 1] =
	vertices[i * VERTEX_SIZE + 6] * g;
      gr->gr_vtmp_buffer[i * VERTEX_SIZE + 2] =
	vertices[i * VERTEX_SIZE + 7] * b;
      gr->gr_vtmp_buffer[i * VERTEX_SIZE + 3] =
	vertices[i * VERTEX_SIZE + 8] * alpha;
    }

    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, sizeof(float) * VERTEX_SIZE,
		   gr->gr_vtmp_buffer);
  } else {
    glColor4f(r, g, b, alpha);
  }

  if(rgb_off != NULL) {
    glEnable(GL_COLOR_SUM);
    glSecondaryColor3f(rgb_off->r, rgb_off->g, rgb_off->b);
  }

  if(tex == NULL) {
    glBindTexture(gbr->gbr_primary_texture_mode, 0);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  } else {
    glBindTexture(gbr->gbr_primary_texture_mode, tex->tex);
    glTexCoordPointer(2, GL_FLOAT, sizeof(float) * VERTEX_SIZE,
		      vertices + 3);
  }

  if(indices != NULL)
    glDrawElements(GL_TRIANGLES, 3 * num_triangles,
		   GL_UNSIGNED_SHORT, indices);
  else
    glDrawArrays(GL_TRIANGLES, 0, num_vertices);

  if(rgb_off != NULL)
    glDisable(GL_COLOR_SUM);

  if(flags & GLW_RENDER_COLOR_ATTRIBUTES)
    glDisableClientState(GL_COLOR_ARRAY);

  if(tex == NULL)
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
}


/**
 * Render function using OpenGL shaders
 */
static void
shader_render(struct glw_root *root, 
	      Mtx m,
	      struct glw_backend_texture *tex,
	      const struct glw_rgb *rgb_mul,
	      const struct glw_rgb *rgb_off,
	      float alpha, float blur,
	      const float *vertices,
	      int num_vertices,
	      const uint16_t *indices,
	      int num_triangles,
	      int flags)
{
  glw_backend_root_t *gbr = &root->gr_be;
  glw_program_t *gp;

  if(tex == NULL) {
    gp = gbr->gbr_renderer_flat;
  } else {
    
    if(blur > 0.05) {
      gp = gbr->gbr_renderer_tex_blur;
    } else {
      gp = gbr->gbr_renderer_tex;
    }
    glBindTexture(gbr->gbr_primary_texture_mode, tex->tex);
  }

  if(gp == NULL)
    return;

  glw_load_program(gbr, gp);

  if(rgb_off != NULL)
    glUniform4f(gp->gp_uniform_color_offset,
		rgb_off->r, rgb_off->g, rgb_off->b, 0);
  else
    glUniform4f(gp->gp_uniform_color_offset, 0,0,0,0);

  switch(gbr->be_blendmode) {
  case GLW_BLEND_NORMAL:
    glw_program_set_uniform_color(gbr, rgb_mul->r, rgb_mul->g, rgb_mul->b,
				  alpha);
    break;
  case GLW_BLEND_ADDITIVE:
    glw_program_set_uniform_color(gbr, 
				  rgb_mul->r * alpha,
				  rgb_mul->g * alpha,
				  rgb_mul->b * alpha,
				  1);
    break;
  }

  if(blur > 0.05 && tex != NULL) {
    glUniform2f(gp->gp_uniform_blur_amount, 
		1.5 * blur / tex->width,
		1.5 * blur / tex->height);
  }

  glLoadMatrixf(glw_mtx_get(m) ?: identitymtx);

  glVertexPointer(3,   GL_FLOAT, sizeof(float) * VERTEX_SIZE, vertices);
  glColorPointer(4,    GL_FLOAT, sizeof(float) * VERTEX_SIZE, vertices + 5);
  glTexCoordPointer(2, GL_FLOAT, sizeof(float) * VERTEX_SIZE, vertices + 3);
  
  if(indices != NULL)
    glDrawElements(GL_TRIANGLES, num_triangles * 3,
		   GL_UNSIGNED_SHORT, indices);
  else
    glDrawArrays(GL_TRIANGLES, 0, num_vertices);
}


/**
 *
 */
void
glw_blendmode(struct glw_root *gr, int mode)
{
  if(mode == gr->gr_be.be_blendmode)
    return;
  gr->gr_be.be_blendmode = mode;

  switch(mode) {
  case GLW_BLEND_NORMAL:
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    break;

  case GLW_BLEND_ADDITIVE:
    glBlendFunc(GL_SRC_COLOR, GL_ONE);
    break;
  }
}


/**
 *
 */
GLuint
glw_compile_shader(const char *url, int type)
{
  char *src;
  struct fa_stat st;
  GLint v, len;
  GLuint s;
  char log[4096];
  
  if((src = fa_quickload(url, &st, NULL, log, sizeof(log))) == NULL) {
    TRACE(TRACE_ERROR, "glw", "Unable to load shader %s -- %s\n",
	  url, log);
    return 0;
  }
  
  s = glCreateShader(type);
  glShaderSource(s, 1, (const char **)&src, NULL);
  
  glCompileShader(s);
  glGetShaderInfoLog(s, sizeof(log), &len, log); 
  glGetShaderiv(s, GL_COMPILE_STATUS, &v);
    
  free(src);

  if(!v) {
    TRACE(TRACE_ERROR, "GLW", "Unable to compile shader %s", url);
    TRACE(TRACE_ERROR, "GLW", "%s", log);
    return 0;
  }
  return s;
}


/**
 *
 */
glw_program_t *
glw_make_program(glw_backend_root_t *gbr, const char *title,
		 GLuint vs, GLuint fs)
{
  char log[4096];
  GLsizei len;
  GLint v;
  GLuint p;
  int i;

  p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);

  glGetProgramInfoLog(p, sizeof(log), &len, log); 

  glGetProgramiv(p, GL_LINK_STATUS, &v);
  if(!v) {
    TRACE(TRACE_ERROR, "GLW", "Unable to link shader %s", title);
    TRACE(TRACE_ERROR, "GLW", "%s", log);
    return NULL;
  }

  glw_program_t *gp = calloc(1, sizeof(glw_program_t));

  gp->gp_title = strdup(title);
  gp->gp_program = p;

  glUseProgram(p);
  gbr->gbr_current = gp;

  gp->gp_uniform_color      = glGetUniformLocation(p, "u_color");
  gp->gp_uniform_colormtx   = glGetUniformLocation(p, "u_colormtx");
  gp->gp_uniform_blend      = glGetUniformLocation(p, "u_blend");
  gp->gp_uniform_color_offset= glGetUniformLocation(p, "u_color_offset");
  gp->gp_uniform_blur_amount = glGetUniformLocation(p, "u_blur_amount");
  
#ifdef DEBUG_SHADERS
  printf("Loaded %s\n", title);
  printf("  u_modelview = %d\n", gp->gp_uniform_modelview);
  printf("  u_color     = %d\n", gp->gp_uniform_color);
  printf("  u_colormtx  = %d\n", gp->gp_uniform_colormtx);
  printf("  u_blend     = %d\n", gp->gp_uniform_blend);
  printf("  u_color_offset = %d\n", gp->gp_uniform_color_offset);
#endif

  for(i = 0; i < 6; i++) {
    char name[8];
    snprintf(name, sizeof(name), "u_t%d", i);
    gp->gp_uniform_t[i]         = glGetUniformLocation(p, name);
    if(gp->gp_uniform_t[i] != -1)
      glUniform1i(gp->gp_uniform_t[i], i);
#ifdef DEBUG_SHADERS
    printf("  u_t%d       = %d\n", i, gp->gp_uniform_t[i]);
#endif
  }

  return gp;
}


/**
 *
 */
void
glw_load_program(glw_backend_root_t *gbr, glw_program_t *gp)
{
  if(gbr->gbr_current == gp)
    return;

  gbr->gbr_current = gp;

  if(gp == NULL) {
    glUseProgram(0);
    return;
  }

  glUseProgram(gp->gp_program);
}


/**
 *
 */
void
glw_program_set_uniform_color(glw_backend_root_t *gbr,
			      float r, float g, float b, float a)
{
  glUniform4f(gbr->gbr_current->gp_uniform_color, r, g, b, a);
}



/**
 *
 */
int
glw_opengl_init_context(glw_root_t *gr)
{
  GLint tu = 0;
  glw_backend_root_t *gbr = &gr->gr_be;
  const	GLubyte	*s;
  /* Check OpenGL extensions we would like to have */

  s = glGetString(GL_EXTENSIONS);

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_CULL_FACE);
  gbr->gbr_culling = 1;

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  if(check_gl_ext(s, "GL_ARB_texture_non_power_of_two")) {
    gbr->gbr_texmode = GLW_OPENGL_TEXTURE_NPOT;
    gbr->gbr_primary_texture_mode = GL_TEXTURE_2D;
    gr->gr_normalized_texture_coords = 1;

#ifdef GL_TEXTURE_RECTANGLE_ARB
  } else if(check_gl_ext(s, "GL_ARB_texture_rectangle")) {
    gbr->gbr_texmode = GLW_OPENGL_TEXTURE_RECTANGLE;
    gbr->gbr_primary_texture_mode = GL_TEXTURE_RECTANGLE_ARB;
#endif

  } else {
    gbr->gbr_texmode = GLW_OPENGL_TEXTURE_SIMPLE;
    gbr->gbr_primary_texture_mode = GL_TEXTURE_2D;
    gr->gr_normalized_texture_coords = 1;
    
  }

  glEnable(gbr->gbr_primary_texture_mode);


  glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &tu);
  if(tu < 6) {
    TRACE(TRACE_ERROR, "GLW", 
	  "Insufficient number of texture image units %d < 6 "
	  "for GLW video rendering widget.",
	  tu);
    return -1;
  }
  TRACE(TRACE_DEBUG, "GLW", "%d texture image units available", tu);

  const char *vendor   = (const char *)glGetString(GL_VENDOR);
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  TRACE(TRACE_INFO, "GLW", "OpenGL Renderer: '%s' by '%s'", renderer, vendor);

  int use_shaders = 1;
#if 0
  if(strstr(renderer, "Mesa"))
      use_shaders = 0;
#endif
  if(use_shaders) {

    GLuint vs, fs;

    vs = glw_compile_shader(SHADERPATH"v1.glsl",
			    GL_VERTEX_SHADER);

    fs = glw_compile_shader(SHADERPATH"f_tex.glsl",
			    GL_FRAGMENT_SHADER);
    gbr->gbr_renderer_tex = glw_make_program(gbr, "Texture", vs, fs);
    glDeleteShader(fs);

    fs = glw_compile_shader(SHADERPATH"f_tex_blur.glsl",
			    GL_FRAGMENT_SHADER);
    gbr->gbr_renderer_tex_blur = glw_make_program(gbr, "BlurTexture", vs, fs);
    glDeleteShader(fs);

    fs = glw_compile_shader(SHADERPATH"f_flat.glsl",
			    GL_FRAGMENT_SHADER);
    gbr->gbr_renderer_flat = glw_make_program(gbr, "Flat", vs, fs);
    glDeleteShader(fs);

    glDeleteShader(vs);

    // Video renderer

    vs = glw_compile_shader(SHADERPATH "yuv2rgb_v.glsl",
			    GL_VERTEX_SHADER);


    fs = glw_compile_shader(SHADERPATH "yuv2rgb_1f_norm.glsl",
			    GL_FRAGMENT_SHADER);
    gbr->gbr_yuv2rgb_1f = glw_make_program(gbr, "yuv2rgb_1f_norm", vs, fs);
    glDeleteShader(fs);

    fs = glw_compile_shader(SHADERPATH "yuv2rgb_2f_norm.glsl",
			    GL_FRAGMENT_SHADER);
    gbr->gbr_yuv2rgb_2f = glw_make_program(gbr, "yuv2rgb_2f_norm", vs, fs);
    glDeleteShader(fs);

    glDeleteShader(vs);


    gr->gr_render = shader_render;
    prop_set_string(prop_create(gr->gr_uii.uii_prop, "rendermode"),
		    "OpenGL VP/FP shaders");

    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

  } else {

    gr->gr_set_hw_clipper = hw_set_clip_conf;
    gr->gr_clr_hw_clipper = hw_clr_clip_conf;
    
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    gr->gr_render = ff_render;

    prop_set_string(prop_create(gr->gr_uii.uii_prop, "rendermode"),
		    "OpenGL fixed function");

  }
    
  glMatrixMode(GL_PROJECTION);
  glLoadMatrixf(projection);
  glMatrixMode(GL_MODELVIEW);
  return 0;
}

/**
 *
 */
void
glw_frontface(struct glw_root *gr, int how)
{
  glFrontFace(how == GLW_CW ? GL_CW : GL_CCW);
}
