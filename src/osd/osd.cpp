/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - osd.cpp                                                 *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2008 Nmn Ebenblues                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <SDL.h>
// On-screen Display
#include "gl3w.h"
#include <SDL_thread.h>
#include <SDL_ttf.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "api/m64p_types.h"
#include "osd.h"

extern "C" {
    #define M64P_CORE_PROTOTYPES 1
    #include "api/callbacks.h"
    #include "api/config.h"
    #include "api/m64p_config.h"
    #include "api/m64p_vidext.h"
    #include "api/vidext.h"
    #include "main/list.h"
    #include "main/main.h"
    #include "osal/files.h"
    #include "osal/preproc.h"
    #include "plugin/plugin.h"
}

GLuint program;

GLuint vao[1];
GLuint vbo[2];

GLfloat vertcoords[8]; // set in procedure
const char * gl3_vertshader =
"attribute vec2 position;              \n"
"attribute vec2 coord;                 \n"
"varying vec2 fragcoord;               \n"
"uniform vec2 scaleport;               \n"

"void main(void) {                     \n"
"    vec2 pos = position*scaleport;    \n"
"    pos -= 1.0;                       \n"
"    gl_Position = vec4(pos, 1.0, 1.0);\n"
"    fragcoord = coord;                \n"
"}";

GLfloat texcoords[] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    0.0f, 1.0f,
    1.0f, 1.0f
};
const char * gl3_fragshader =
"varying vec2 fragcoord;                     \n"
"uniform sampler2D texture;                  \n"

"void main(void) {                           \n"
"    vec4 t = texture2D(texture, fragcoord); \n"
"    t.rgb *= t.a;                           \n"
"    gl_FragColor = vec4(t.r, t.g, t.b, t.a);\n"
"}";

struct Font {
    float red;
    float green;
    float blue;
    float alpha;
    TTF_Font * font = NULL;
    float height() { return TTF_FontHeight(font); }
    bool isValid() { return font != NULL; }
    void setForegroundColor
    (   float _red   = 0.0,
        float _green = 0.0,
        float _blue  = 0.0,
        float _alpha = 1.0 )
    {
        red   = _red;
        green = _green;
        blue  = _blue;
        alpha = _alpha;
    }
    void draw (int align, float x, float y, const char * text, float sizebox[4])
    {
        SDL_Surface * temp = TTF_RenderUTF8_Blended(font, text, {255,255,255});
        if(!temp) return;
        
        GLuint texture;
        glUniform1i(glGetUniformLocation(program, "texture"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, temp->w, temp->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, temp->pixels);
        
        if(align == OSD_TOP_CENTER  or align == OSD_MIDDLE_CENTER or align == OSD_BOTTOM_CENTER) x -= temp->w/2;
        if(align == OSD_TOP_RIGHT   or align == OSD_MIDDLE_RIGHT  or align == OSD_BOTTOM_RIGHT)  x -= temp->w;
        if(align == OSD_MIDDLE_LEFT or align == OSD_MIDDLE_CENTER or align == OSD_MIDDLE_RIGHT)  y -= temp->h/2;
        //if(align == OSD_TOP_LEFT    or align == OSD_TOP_CENTER    or align == OSD_TOP_RIGHT)     y -= temp->h;
        if(align == OSD_BOTTOM_LEFT    or align == OSD_BOTTOM_CENTER    or align == OSD_BOTTOM_RIGHT)     y -= temp->h;
        
        vertcoords[0] = x;
        vertcoords[1] = y;
        vertcoords[2] = x+temp->w;
        vertcoords[3] = y;
        vertcoords[4] = x;
        vertcoords[5] = y+temp->h;
        vertcoords[6] = x+temp->w;
        vertcoords[7] = y+temp->h;
        SDL_FreeSurface(temp);
        
        glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertcoords), vertcoords);
        
        glEnable(GL_BLEND);
        glBlendColor(red, green, blue, alpha); // TODO: test
        glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
};

Font * l_font;

#define FONT_FILENAME "font.ttf"

typedef void (APIENTRYP PTRGLACTIVETEXTURE)(GLenum texture);
static PTRGLACTIVETEXTURE pglActiveTexture = NULL;

// static variables for OSD
static int l_OsdInitialized = 0;

static LIST_HEAD(l_messageQueue);
static float l_fLineHeight = -1.0;

static void animation_none(osd_message_t *);
static void animation_fade(osd_message_t *);
static void osd_remove_message(osd_message_t *msg);
static osd_message_t * osd_message_valid(osd_message_t *testmsg);

static float fCornerScroll[OSD_NUM_CORNERS];

static SDL_mutex *osd_list_lock;

// animation handlers
static void (*l_animations[OSD_NUM_ANIM_TYPES])(osd_message_t *) = {
    animation_none, // animation handler for OSD_NONE
    animation_fade  // animation handler for OSD_FADE
};

// private functions
// draw message on screen
static void draw_message(osd_message_t *msg, int width, int height)
{
    float x = 0.,
          y = 0.;

    if(!l_font || !l_font->isValid())
        return;

    // set justification based on corner
    switch(msg->corner)
    {
        case OSD_TOP_LEFT:
            x = 0.;
            y = (float)height;
            break;
        case OSD_TOP_CENTER:
            x = ((float)width)/2.0f;
            y = (float)height;
            break;
        case OSD_TOP_RIGHT:
            x = (float)width;
            y = (float)height;
            break;
        case OSD_MIDDLE_LEFT:
            x = 0.;
            y = ((float)height)/2.0f;
            break;
        case OSD_MIDDLE_CENTER:
            x = ((float)width)/2.0f;
            y = ((float)height)/2.0f;
            break;
        case OSD_MIDDLE_RIGHT:
            x = (float)width;
            y = ((float)height)/2.0f;
            break;
        case OSD_BOTTOM_LEFT:
            x = 0.;
            y = 0.;
            break;
        case OSD_BOTTOM_CENTER:
            x = ((float)width)/2.0f;
            y = 0.;
            break;
        case OSD_BOTTOM_RIGHT:
            x = (float)width;
            y = 0.;
            break;
        default:
            x = 0.;
            y = 0.;
            break;
    }   
    // apply animation for current message state
    (*l_animations[msg->animation[msg->state]])(msg);

    // xoffset moves message left
    x -= msg->xoffset;
    // yoffset moves message up
    y += msg->yoffset;

    // draw the text line
    l_font->draw(msg->corner, x, y, msg->text, msg->sizebox);
}

// null animation handler
static void animation_none(osd_message_t *msg) { }

// fade in/out animation handler
static void animation_fade(osd_message_t *msg)
{
    float alpha = 1.;
    float elapsed_frames;
    float total_frames = (float)msg->timeout[msg->state];

    switch(msg->state)
    {
        case OSD_DISAPPEAR:
            elapsed_frames = (float)(total_frames - msg->frames);
            break;
        case OSD_APPEAR:
        default:
            elapsed_frames = (float)msg->frames;
            break;
    }

    if(total_frames != 0.)
        alpha = elapsed_frames / total_frames;

    l_font->setForegroundColor(msg->color[R], msg->color[G], msg->color[B], alpha);
}

// sets message Y offset depending on where they are in the message queue
static float get_message_offset(osd_message_t *msg, float fLinePos)
{
    float offset = (float) (l_font->height() * fLinePos);

    switch(msg->corner)
    {
        case OSD_TOP_LEFT:
        case OSD_TOP_CENTER:
        case OSD_TOP_RIGHT:
            return -offset;
            break;
        default:
            return offset;
            break;
    }
}

// public functions
extern "C"
void osd_init(int width, int height)
{
    const char *fontpath;
    if(gl3wInit()) {
        DebugMessage(M64MSG_ERROR, "Failed to load gl3w");
        return;
    }
    if(TTF_Init()) {
        DebugMessage(M64MSG_ERROR, "Failed to load SDL2_TTF");
        return;
    }

    osd_list_lock = SDL_CreateMutex();
    if (!osd_list_lock) {
        DebugMessage(M64MSG_ERROR, "Could not create osd list lock");
        return;
    }

    fontpath = ConfigGetSharedDataFilepath(FONT_FILENAME);
    
    l_font = new Font;
    l_font->font = TTF_OpenFont(fontpath, height / 35.0f);
    puts(SDL_GetError());
    
    if(!l_font || !l_font->isValid())
    {
        DebugMessage(M64MSG_ERROR, "Could not load font from %s", fontpath);
        return;
    }

    auto vert_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vert_shader, 1, &gl3_vertshader, NULL);
    glCompileShader(vert_shader);
    
    GLint success;
    glGetShaderiv(vert_shader, GL_COMPILE_STATUS, &success);
    if(!success)
    {
        GLchar infoLog[512];
        glGetShaderInfoLog(vert_shader, 512, NULL, infoLog);
        DebugMessage(M64MSG_ERROR, "Failed to compile vert shader. %s", infoLog);
        assert(false);
    }
    
    auto frag_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(frag_shader, 1, &gl3_fragshader, NULL);
    glCompileShader(frag_shader);
    
    program = glCreateProgram();
    glAttachShader(program, vert_shader);
    glAttachShader(program, frag_shader);
    glLinkProgram(program);
    
    glDeleteShader(vert_shader);
    glDeleteShader(frag_shader);
    
    glGenVertexArrays(1, vao);
    glGenBuffers(2, vbo);
    
    GLint former_vao, former_vbo;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &former_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &former_vbo);
    
    glBindVertexArray(vao[0]);
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo[0]); // rect coord
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertcoords), vertcoords, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo[1]); // tex coord
    glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
    
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    // clear statics
    for (int i = 0; i < OSD_NUM_CORNERS; i++)
        fCornerScroll[i] = 0.0;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    glBindBuffer(GL_ARRAY_BUFFER, former_vbo);
    glBindVertexArray(former_vao);

    pglActiveTexture = (PTRGLACTIVETEXTURE) VidExt_GL_GetProcAddress("glActiveTexture");
    if (pglActiveTexture == NULL)
    {
        DebugMessage(M64MSG_WARNING, "OpenGL function glActiveTexture() not supported.  OSD deactivated.");
        return;
    }

    // set initialized flag
    l_OsdInitialized = 1;
}

extern "C"
void osd_exit(void)
{
    osd_message_t *msg, *safe;
    
    // delete font renderer
    if (l_font)
    {
        TTF_CloseFont(l_font->font);
        delete l_font;
        l_font = NULL;
    }

    // delete message queue
    SDL_LockMutex(osd_list_lock);
    list_for_each_entry_safe_t(msg, safe, &l_messageQueue, osd_message_t, list) {
        osd_remove_message(msg);
        if (!msg->user_managed)
            free(msg);
    }
    SDL_UnlockMutex(osd_list_lock);

    SDL_DestroyMutex(osd_list_lock);

    TTF_Quit();

    // reset initialized flag
    l_OsdInitialized = 0;
}

// renders the current osd message queue to the screen
extern "C"
void osd_render()
{
    osd_message_t *msg, *safe;
    int i;

    // if we're not initialized or list is empty, then just skip it all
    if (!l_OsdInitialized || list_empty(&l_messageQueue))
        return;
    
    GLint former_vao, former_vbo;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &former_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &former_vbo);
    
    glUseProgram(program);
    glBindVertexArray(vao[0]);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
    glDisable(GL_CULL_FACE);

    // get the viewport dimensions
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLfloat scaleport[2];
    scaleport[0] = 2.0f/viewport[2];
    scaleport[1] = -2.0f/viewport[3];
    glUniform2fv(glGetUniformLocation(program, "scaleport"), 1, scaleport);

    // keeps track of next message position for each corner
    float fCornerPos[OSD_NUM_CORNERS];
    for (i = 0; i < OSD_NUM_CORNERS; i++)
        fCornerPos[i] = 0.5f * l_fLineHeight;

    SDL_LockMutex(osd_list_lock);
    list_for_each_entry_safe_t(msg, safe, &l_messageQueue, osd_message_t, list) {
        // update message state
        if(msg->timeout[msg->state] != OSD_INFINITE_TIMEOUT &&
           ++msg->frames >= msg->timeout[msg->state])
        {
            // if message is in last state, mark it for deletion and continue to the next message
            if(msg->state >= OSD_NUM_STATES - 1)
            {
                if (msg->user_managed) {
                    osd_remove_message(msg);
                } else {
                    osd_remove_message(msg);
                    free(msg);
                }

                continue;
            }

            // go to next state and reset frame count
            msg->state++;
            msg->frames = 0;
        }

        // offset y depending on how many other messages are in the same corner
        float fStartOffset;
        if (msg->corner >= OSD_MIDDLE_LEFT && msg->corner <= OSD_MIDDLE_RIGHT)  // don't scroll the middle messages
            fStartOffset = fCornerPos[msg->corner];
        else
            fStartOffset = fCornerPos[msg->corner] + (fCornerScroll[msg->corner] * l_fLineHeight);
            
        msg->yoffset = get_message_offset(msg, fStartOffset);
        msg->xoffset = 0;
        draw_message(msg, viewport[2], viewport[3]);
        
        fCornerPos[msg->corner] += l_fLineHeight;
    }
    SDL_UnlockMutex(osd_list_lock);

    // do the scrolling
    for (int i = 0; i < OSD_NUM_CORNERS; i++)
    {
        fCornerScroll[i] += 0.1f;
        if (fCornerScroll[i] >= 0.0)
            fCornerScroll[i] = 0.0;
    }
    
    glFinish();
    
    glBindBuffer(GL_ARRAY_BUFFER, former_vbo);
    glBindVertexArray(former_vao);
}

// creates a new osd_message_t, adds it to the message queue and returns it in case
// the user wants to modify its parameters. Note, if the message can't be created,
// NULL is returned.
extern "C"
osd_message_t * osd_new_message(enum osd_corner eCorner, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    if (!l_OsdInitialized) return NULL;

    osd_message_t *msg = (osd_message_t *)malloc(sizeof(*msg));

    if (!msg) return NULL;

    va_start(ap, fmt);
    vsnprintf(buf, 1024, fmt, ap);
    buf[1023] = 0;
    va_end(ap);

    // set default values
    memset(msg, 0, sizeof(osd_message_t));
    msg->text = strdup(buf);
    msg->user_managed = 0;
    // default to white
    msg->color[R] = 1.;
    msg->color[G] = 1.;
    msg->color[B] = 1.;

    msg->sizebox[0] = 0.0;  // set a null bounding box
    msg->sizebox[1] = 0.0;
    msg->sizebox[2] = 0.0;
    msg->sizebox[3] = 0.0;

    msg->corner = eCorner;
    msg->state = OSD_APPEAR;
    fCornerScroll[eCorner] -= 1.0;  // start this one before the beginning of the list and scroll it in

    msg->animation[OSD_APPEAR] = OSD_FADE;
    msg->animation[OSD_DISPLAY] = OSD_NONE;
    msg->animation[OSD_DISAPPEAR] = OSD_FADE;

    if (eCorner >= OSD_MIDDLE_LEFT && eCorner <= OSD_MIDDLE_RIGHT)
    {
        msg->timeout[OSD_APPEAR] = 20;
        msg->timeout[OSD_DISPLAY] = 60;
        msg->timeout[OSD_DISAPPEAR] = 20;
    }
    else
    {
        msg->timeout[OSD_APPEAR] = 20;
        msg->timeout[OSD_DISPLAY] = 180;
        msg->timeout[OSD_DISAPPEAR] = 40;
    }

    // add to message queue
    SDL_LockMutex(osd_list_lock);
    list_add(&msg->list, &l_messageQueue);
    SDL_UnlockMutex(osd_list_lock);

    return msg;
}

// update message string
extern "C"
void osd_update_message(osd_message_t *msg, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    if (!l_OsdInitialized || !msg) return;

    va_start(ap, fmt);
    vsnprintf(buf, 1024, fmt, ap);
    buf[1023] = 0;
    va_end(ap);

    free(msg->text);
    msg->text = strdup(buf);

    // reset bounding box
    msg->sizebox[0] = 0.0;
    msg->sizebox[1] = 0.0;
    msg->sizebox[2] = 0.0;
    msg->sizebox[3] = 0.0;

    // reset display time counter
    if (msg->state >= OSD_DISPLAY)
    {
        msg->state = OSD_DISPLAY;
        msg->frames = 0;
    }

    SDL_LockMutex(osd_list_lock);
    if (!osd_message_valid(msg))
        list_add(&msg->list, &l_messageQueue);
    SDL_UnlockMutex(osd_list_lock);

}

// remove message from message queue
static void osd_remove_message(osd_message_t *msg)
{
    if (!l_OsdInitialized || !msg) return;

    free(msg->text);
    msg->text = NULL;
    list_del(&msg->list);
}

// remove message from message queue and free it
extern "C"
void osd_delete_message(osd_message_t *msg)
{
    if (!l_OsdInitialized || !msg) return;

    SDL_LockMutex(osd_list_lock);
    osd_remove_message(msg);
    free(msg);
    SDL_UnlockMutex(osd_list_lock);
}

// set message so it doesn't automatically expire in a certain number of frames.
extern "C"
void osd_message_set_static(osd_message_t *msg)
{
    if (!l_OsdInitialized || !msg) return;

    msg->timeout[OSD_DISPLAY] = OSD_INFINITE_TIMEOUT;
    msg->state = OSD_DISPLAY;
    msg->frames = 0;
}

// set message so it doesn't automatically get freed when finished transition.
extern "C"
void osd_message_set_user_managed(osd_message_t *msg)
{
    if (!l_OsdInitialized || !msg) return;

    msg->user_managed = 1;
}

// return message pointer if valid (in the OSD list), otherwise return NULL
static osd_message_t * osd_message_valid(osd_message_t *testmsg)
{
    osd_message_t *msg;

    if (!l_OsdInitialized || !testmsg) return NULL;

    list_for_each_entry_t(msg, &l_messageQueue, osd_message_t, list) {
        if (msg == testmsg)
            return testmsg;
    }

    return NULL;
}

