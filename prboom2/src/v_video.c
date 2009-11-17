/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *  Gamma correction LUT stuff.
 *  Color range translation support
 *  Functions to draw patches (by post) directly to screen.
 *  Functions to blit a block to the screen.
 *
 *-----------------------------------------------------------------------------
 */

#include "doomdef.h"
#include "r_main.h"
#include "r_draw.h"
#include "m_bbox.h"
#include "w_wad.h"   /* needed for color translation lump lookup */
#include "v_video.h"
#include "i_video.h"
#include "r_filter.h"
#include "lprintf.h"
#include "st_stuff.h"
#include "e6y.h"

const char *render_aspects_list[5] = {"auto", "16:9", "16:10", "4:3", "5:4"};
const char *render_stretch_list[patch_stretch_max] = {"not adjusted", "Doom format", "fit to width"};

stretch_param_t stretch_params_table[3][VPT_ALIGN_MAX];
stretch_param_t *stretch_params;

cb_video_t video;
cb_video_t video_stretch;
cb_video_t video_full;
int patches_scalex;
int patches_scaley;

int render_stretch_hud;
int render_stretch_hud_default;

// Each screen is [SCREENWIDTH*SCREENHEIGHT];
screeninfo_t screens[NUM_SCREENS];

/* jff 4/24/98 initialize this at runtime */
const byte *colrngs[CR_LIMIT];

int usegamma;

/*
 * V_InitColorTranslation
 *
 * Loads the color translation tables from predefined lumps at game start
 * No return
 *
 * Used for translating text colors from the red palette range
 * to other colors. The first nine entries can be used to dynamically
 * switch the output of text color thru the HUlib_drawText routine
 * by embedding ESCn in the text to obtain color n. Symbols for n are
 * provided in v_video.h.
 *
 * cphipps - constness of crdef_t stuff fixed
 */

typedef struct {
  const char *name;
  const byte **map;
} crdef_t;

// killough 5/2/98: table-driven approach
static const crdef_t crdefs[] = {
  {"CRBRICK",  &colrngs[CR_BRICK ]},
  {"CRTAN",    &colrngs[CR_TAN   ]},
  {"CRGRAY",   &colrngs[CR_GRAY  ]},
  {"CRGREEN",  &colrngs[CR_GREEN ]},
  {"CRBROWN",  &colrngs[CR_BROWN ]},
  {"CRGOLD",   &colrngs[CR_GOLD  ]},
  {"CRRED",    &colrngs[CR_RED   ]},
  {"CRBLUE",   &colrngs[CR_BLUE  ]},
  {"CRORANGE", &colrngs[CR_ORANGE]},
  {"CRYELLOW", &colrngs[CR_YELLOW]},
  {"CRBLUE2",  &colrngs[CR_BLUE2]},
  {NULL}
};

// killough 5/2/98: tiny engine driven by table above
void V_InitColorTranslation(void)
{
  register const crdef_t *p;
  for (p=crdefs; p->name; p++)
    *p->map = W_CacheLumpName(p->name);
}

//
// V_CopyRect
//
// Copies a source rectangle in a screen buffer to a destination
// rectangle in another screen buffer. Source origin in srcx,srcy,
// destination origin in destx,desty, common size in width and height.
// Source buffer specfified by srcscrn, destination buffer by destscrn.
//
// Marks the destination rectangle on the screen dirty.
//
// No return.
//
static void FUNC_V_CopyRect(int srcx, int srcy, int srcscrn, int width,
                int height, int destx, int desty, int destscrn,
                enum patch_translation_e flags)
{
  byte *src;
  byte *dest;

  if (flags & VPT_STRETCH_MASK)
  {
    stretch_param_t *params;
    int sx = srcx;
    int w = width;
    int sy = srcy;
    int h = height;

    params = &stretch_params[flags & VPT_ALIGN_MASK];

#if 1
    srcx   = params->video->x1lookup[srcx];
    srcy   = params->video->y1lookup[srcy];
#if 0
    width  = params->video->x1lookup[sx + w] - srcx;
    height = params->video->y1lookup[sy + h] - srcy;
#else
    width  = params->video->x2lookup[sx + w - 1] - srcx;
    height = params->video->y2lookup[sy + h - 1] - srcy;
#endif
    destx  = params->video->x1lookup[destx] + params->deltax1;
    desty  = params->video->y1lookup[desty] + params->deltay1;
#else
    srcx=srcx*WIDE_SCREENWIDTH/320 + deltax;
    srcy=srcy*SCREENHEIGHT/200;
    width=width*WIDE_SCREENWIDTH/320;
    height=height*SCREENHEIGHT/200;
    destx=destx*WIDE_SCREENWIDTH/320 + deltax;
    desty=desty*SCREENHEIGHT/200;
#endif
  }

#ifdef RANGECHECK
  if (srcx<0
      ||srcx+width >SCREENWIDTH
      || srcy<0
      || srcy+height>SCREENHEIGHT
      ||destx<0||destx+width >SCREENWIDTH
      || desty<0
      || desty+height>SCREENHEIGHT)
    I_Error ("V_CopyRect: Bad arguments");
#endif

  src = screens[srcscrn].data+screens[srcscrn].byte_pitch*srcy+srcx*V_GetPixelDepth();
  dest = screens[destscrn].data+screens[destscrn].byte_pitch*desty+destx*V_GetPixelDepth();

  for ( ; height>0 ; height--)
    {
      memcpy (dest, src, width*V_GetPixelDepth());
      src += screens[srcscrn].byte_pitch;
      dest += screens[destscrn].byte_pitch;
    }
}

static void FUNC_V_FillFlat(int lump, int scrn, int x, int y, int width, int height, enum patch_translation_e flags)
{
  /* erase the entire screen to a tiled background */
  const byte *src;
  int         sx, sy;
  int         w, h;

  lump += firstflat;

  // killough 4/17/98:
  src = W_CacheLumpNum(lump);

  w = h = 64;
  if (V_GetMode() == VID_MODE8) {
    byte *dest = screens[scrn].data;

    while (h--) {
      memcpy (dest, src, w);
      src += w;
      dest += screens[scrn].byte_pitch;
    }
  } else if (V_GetMode() == VID_MODE15) {
    unsigned short *dest = (unsigned short *)screens[scrn].data;

    while (h--) {
      int i;
      for (i=0; i<w; i++) {
        dest[i] = VID_PAL15(src[i], VID_COLORWEIGHTMASK);
      }
      src += w;
      dest += screens[scrn].short_pitch;
    }
  } else if (V_GetMode() == VID_MODE16) {
    unsigned short *dest = (unsigned short *)screens[scrn].data;

    while (h--) {
      int i;
      for (i=0; i<w; i++) {
        dest[i] = VID_PAL16(src[i], VID_COLORWEIGHTMASK);
      }
      src += w;
      dest += screens[scrn].short_pitch;
    }
  } else if (V_GetMode() == VID_MODE32) {
    unsigned int *dest = (unsigned int *)screens[scrn].data;

    while (h--) {
      int i;
      for (i=0; i<w; i++) {
        dest[i] = VID_PAL32(src[i], VID_COLORWEIGHTMASK);
      }
      src += w;
      dest += screens[scrn].int_pitch;
    }
  }
  /* end V_DrawBlock */

  for (sy = y ; sy < y + height; sy += 64)
  {
    for (sx = x/*sy ? x : x + 64*/; sx < x + width; sx += 64)
    {
      V_CopyRect(0, 0, scrn,
      (x + width - sx < 64 ? x + width - sx : 64),
      (y + height - sy < 64 ? y + height - sy : 64),
      sx, sy, scrn, VPT_NONE);
    }
  }
  W_UnlockLumpNum(lump);
}

static void FUNC_V_FillPatch(int lump, int scrn, int x, int y, int width, int height, enum patch_translation_e flags)
{
  int sx, sy, w, h;

  w = R_NumPatchWidth(lump);
  h = R_NumPatchHeight(lump);

  for (sy = y; sy < y + height; sy += h)
  {
    for (sx = x; sx < x + width; sx += w)
    {
      V_DrawNumPatch(sx, sy, scrn, lump, CR_DEFAULT, flags);
    }
  }
}

/*
 * V_DrawBackground tiles a 64x64 patch over the entire screen, providing the
 * background for the Help and Setup screens, and plot text betwen levels.
 * cphipps - used to have M_DrawBackground, but that was used the framebuffer
 * directly, so this is my code from the equivalent function in f_finale.c
 */
static void FUNC_V_DrawBackground(const char* flatname, int scrn)
{
  V_FillFlatName(flatname, scrn, 0, 0, SCREENWIDTH, SCREENHEIGHT, VPT_NONE);
}

//
// V_Init
//
// Allocates the 4 full screen buffers in low DOS memory
// No return
//

void V_Init (void)
{
  int  i;

  // reset the all
  for (i = 0; i<NUM_SCREENS; i++) {
    screens[i].data = NULL;
    screens[i].not_on_heap = false;
    screens[i].width = 0;
    screens[i].height = 0;
    screens[i].byte_pitch = 0;
    screens[i].short_pitch = 0;
    screens[i].int_pitch = 0;
  }
}

//
// V_DrawMemPatch
//
// CPhipps - unifying patch drawing routine, handles all cases and combinations
//  of stretching, flipping and translating
//
// This function is big, hopefully not too big that gcc can't optimise it well.
// In fact it packs pretty well, there is no big performance lose for all this merging;
// the inner loops themselves are just the same as they always were
// (indeed, laziness of the people who wrote the 'clones' of the original V_DrawPatch
//  means that their inner loops weren't so well optimised, so merging code may even speed them).
//
static void V_DrawMemPatch(int x, int y, int scrn, const rpatch_t *patch,
        int cm, enum patch_translation_e flags)
{
  const byte *trans;

  stretch_param_t *params;

  if (cm<CR_LIMIT)
    trans=colrngs[cm];
  else
    trans=translationtables + 256*((cm-CR_LIMIT)-1);
  y -= patch->topoffset;
  x -= patch->leftoffset;

  // CPhipps - auto-no-stretch if not high-res
  if (flags & VPT_STRETCH_MASK)
    if ((SCREENWIDTH==320) && (SCREENHEIGHT==200) && (SCREENWIDTH==WIDE_SCREENWIDTH))
      flags &= ~VPT_STRETCH_MASK;

  // e6y: wide-res
  params = &stretch_params[flags & VPT_ALIGN_MASK];

  // CPhipps - null translation pointer => no translation
  if (!trans)
    flags &= ~VPT_TRANS;

  if (V_GetMode() == VID_MODE8 && !(flags & VPT_STRETCH_MASK)) {
    int             col;
    byte           *desttop = screens[scrn].data+y*screens[scrn].byte_pitch+x*V_GetPixelDepth();
    int    w = patch->width;

    if (y<0 || y+patch->height > ((flags & VPT_STRETCH) ? 200 :  SCREENHEIGHT)) {
      // killough 1/19/98: improved error message:
      lprintf(LO_WARN, "V_DrawMemPatch8: Patch (%d,%d)-(%d,%d) exceeds LFB in vertical direction (horizontal is clipped)\n"
              "Bad V_DrawMemPatch8 (flags=%u)", x, y, x+patch->width, y+patch->height, flags);
      return;
    }

    w--; // CPhipps - note: w = width-1 now, speeds up flipping

    for (col=0 ; col<=w ; desttop++, col++, x++) {
      int i;
      const int colindex = (flags & VPT_FLIP) ? (w - col) : (col);
      const rcolumn_t *column = R_GetPatchColumn(patch, colindex);

      if (x < 0)
        continue;
      if (x >= SCREENWIDTH)
        break;

      // step through the posts in a column
      for (i=0; i<column->numPosts; i++) {
        const rpost_t *post = &column->posts[i];
        // killough 2/21/98: Unrolled and performance-tuned

        const byte *source = column->pixels + post->topdelta;
        byte *dest = desttop + post->topdelta*screens[scrn].byte_pitch;
        int count = post->length;

        if (!(flags & VPT_TRANS)) {
          if ((count-=4)>=0)
            do {
              register byte s0,s1;
              s0 = source[0];
              s1 = source[1];
              dest[0] = s0;
              dest[screens[scrn].byte_pitch] = s1;
              dest += screens[scrn].byte_pitch*2;
              s0 = source[2];
              s1 = source[3];
              source += 4;
              dest[0] = s0;
              dest[screens[scrn].byte_pitch] = s1;
              dest += screens[scrn].byte_pitch*2;
            } while ((count-=4)>=0);
          if (count+=4)
            do {
              *dest = *source++;
              dest += screens[scrn].byte_pitch;
            } while (--count);
        } else {
          // CPhipps - merged translation code here
          if ((count-=4)>=0)
            do {
              register byte s0,s1;
              s0 = source[0];
              s1 = source[1];
              s0 = trans[s0];
              s1 = trans[s1];
              dest[0] = s0;
              dest[screens[scrn].byte_pitch] = s1;
              dest += screens[scrn].byte_pitch*2;
              s0 = source[2];
              s1 = source[3];
              s0 = trans[s0];
              s1 = trans[s1];
              source += 4;
              dest[0] = s0;
              dest[screens[scrn].byte_pitch] = s1;
              dest += screens[scrn].byte_pitch*2;
            } while ((count-=4)>=0);
          if (count+=4)
            do {
              *dest = trans[*source++];
              dest += screens[scrn].byte_pitch;
            } while (--count);
        }
      }
    }
  }
  else {
    // CPhipps - move stretched patch drawing code here
    //         - reformat initialisers, move variables into inner blocks

    int   col;
    int   w = (patch->width << 16) - 1; // CPhipps - -1 for faster flipping
    int   left, right, top, bottom;
    int   DXI, DYI;
    R_DrawColumn_f colfunc;
    draw_column_vars_t dcvars;
    draw_vars_t olddrawvars = drawvars;

    R_SetDefaultDrawColumnVars(&dcvars);

    drawvars.byte_topleft = screens[scrn].data;
    drawvars.short_topleft = (unsigned short *)screens[scrn].data;
    drawvars.int_topleft = (unsigned int *)screens[scrn].data;
    drawvars.byte_pitch = screens[scrn].byte_pitch;
    drawvars.short_pitch = screens[scrn].short_pitch;
    drawvars.int_pitch = screens[scrn].int_pitch;

    if (flags & VPT_TRANS) {
      colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED, drawvars.filterpatch, RDRAW_FILTER_NONE);
      dcvars.translation = trans;
    } else {
      colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD, drawvars.filterpatch, RDRAW_FILTER_NONE);
    }

    //e6y: predefined arrays are used
    if (!(flags & VPT_STRETCH_MASK))
    {
      DXI = 1 << 16;
      DYI = 1 << 16;

      left = x;
      top = y;
      right = x + patch->width - 1;
      bottom = y + patch->height;
    }
    else
    {
      DXI = params->video->xstep;
      DYI = params->video->ystep;

      //FIXME: Is it needed only for F_BunnyScroll?

      left = (x < 0 || x > 320 ? (x * params->video->width) / 320 : params->video->x1lookup[x]);
      top =  (y < 0 || y > 200 ? (y * params->video->height) / 200 : params->video->y1lookup[y]);

      if (x + patch->width < 0 || x + patch->width > 320)
        right = ( ((x + patch->width - 1) * params->video->width) / 320 );
      else
        right = params->video->x2lookup[x + patch->width - 1];

      if (y + patch->height < 0 || y + patch->height > 200)
        bottom = ( ((y + patch->height - 0) * params->video->height) / 200 );
      else
        bottom = params->video->y2lookup[y + patch->height - 1];

      left   += params->deltax1;
      right  += params->deltax2;
      top    += params->deltay1;
      bottom += params->deltay1;
    }

    dcvars.texheight = patch->height;
    dcvars.iscale = DYI;
    dcvars.drawingmasked = MAX(patch->width, patch->height) > 8;
    dcvars.edgetype = drawvars.patch_edges;

    if (drawvars.filterpatch == RDRAW_FILTER_LINEAR) {
      // bias the texture u coordinate
      if (patch->flags&PATCH_ISNOTTILEABLE)
        col = -(FRACUNIT>>1);
      else
        col = (patch->width<<FRACBITS)-(FRACUNIT>>1);
    }
    else {
      col = 0;
    }

    for (dcvars.x=left; dcvars.x<=right; dcvars.x++, col+=DXI) {
      int i;
      const int colindex = (flags & VPT_FLIP) ? ((w - col)>>16): (col>>16);
      const rcolumn_t *column = R_GetPatchColumn(patch, colindex);
      const rcolumn_t *prevcolumn = R_GetPatchColumn(patch, colindex-1);
      const rcolumn_t *nextcolumn = R_GetPatchColumn(patch, colindex+1);

      // ignore this column if it's to the left of our clampRect
      if (dcvars.x < 0)
        continue;
      if (dcvars.x >= SCREENWIDTH)
        break;

      dcvars.texu = ((flags & VPT_FLIP) ? ((patch->width<<FRACBITS)-col) : col) % (patch->width<<FRACBITS);

      // step through the posts in a column
      for (i=0; i<column->numPosts; i++) {
        const rpost_t *post = &column->posts[i];
        int yoffset = 0;

        //e6y
        if (!(flags & VPT_STRETCH_MASK))
        {
          dcvars.yl = y + post->topdelta;
          dcvars.yh = ((((y + post->topdelta + post->length) << 16) - (FRACUNIT>>1))>>FRACBITS);
        }
        else
        {
          // e6y
          // More accurate patch drawing from Eternity.
          // Predefined arrays are used instead of dynamic calculation 
          // of the top and bottom screen coordinates of a column.
          // Also, it should be faster.
          dcvars.yl = params->video->y1lookup[y + post->topdelta] + params->deltay1;
          dcvars.yh = params->video->y2lookup[y + post->topdelta + post->length - 1] + params->deltay1;
        }
        dcvars.edgeslope = post->slope;

        if ((dcvars.yh < 0) || (dcvars.yh < top))
          continue;
        if ((dcvars.yl >= SCREENHEIGHT) || (dcvars.yl >= bottom))
          continue;

        if (dcvars.yh >= bottom) {
          //dcvars.yh = bottom-1;
          dcvars.edgeslope &= ~RDRAW_EDGESLOPE_BOT_MASK;
        }
        if (dcvars.yh >= SCREENHEIGHT) {
          dcvars.yh = SCREENHEIGHT-1;
          dcvars.edgeslope &= ~RDRAW_EDGESLOPE_BOT_MASK;
        }

        if (dcvars.yl < 0) {
          yoffset = 0-dcvars.yl;
          dcvars.yl = 0;
          dcvars.edgeslope &= ~RDRAW_EDGESLOPE_TOP_MASK;
        }
        if (dcvars.yl < top) {
          yoffset = top-dcvars.yl;
          dcvars.yl = top;
          dcvars.edgeslope &= ~RDRAW_EDGESLOPE_TOP_MASK;
        }

        dcvars.source = column->pixels + post->topdelta + yoffset;
        dcvars.prevsource = prevcolumn ? prevcolumn->pixels + post->topdelta + yoffset: dcvars.source;
        dcvars.nextsource = nextcolumn ? nextcolumn->pixels + post->topdelta + yoffset: dcvars.source;

        dcvars.texturemid = -((dcvars.yl-centery)*dcvars.iscale);

        //e6y
        dcvars.dy = params->deltay1;
        dcvars.flags |= DRAW_COLUMN_ISPATCH; 

        colfunc(&dcvars);
      }
    }

    R_ResetColumnBuffer();
    drawvars = olddrawvars;
  }
}

// CPhipps - some simple, useful wrappers for that function, for drawing patches from wads

// CPhipps - GNU C only suppresses generating a copy of a function if it is
// static inline; other compilers have different behaviour.
// This inline is _only_ for the function below

static void FUNC_V_DrawNumPatch(int x, int y, int scrn, int lump,
         int cm, enum patch_translation_e flags)
{
  V_DrawMemPatch(x, y, scrn, R_CachePatchNum(lump), cm, flags);
  R_UnlockPatchNum(lump);
}

unsigned short *V_Palette15 = NULL;
unsigned short *V_Palette16 = NULL;
unsigned int *V_Palette32 = NULL;
static unsigned short *Palettes15 = NULL;
static unsigned short *Palettes16 = NULL;
static unsigned int *Palettes32 = NULL;
static int currentPaletteIndex = 0;

//
// V_UpdateTrueColorPalette
//
void V_UpdateTrueColorPalette(video_mode_t mode) {
  int i, w, p;
  byte r,g,b;
  int nr,ng,nb;
  float t;
  int paletteNum = (V_GetMode() == VID_MODEGL ? 0 : currentPaletteIndex);
  static int usegammaOnLastPaletteGeneration = -1;
  
  int pplump = W_GetNumForName("PLAYPAL");
  int gtlump = (W_CheckNumForName)("GAMMATBL",ns_prboom);
  const byte *pal = W_CacheLumpNum(pplump);
  // opengl doesn't use the gamma
  const byte *const gtable = 
    (const byte *)W_CacheLumpNum(gtlump) + 
    (V_GetMode() == VID_MODEGL ? 0 : 256*(usegamma))
  ;

  int numPals = W_LumpLength(pplump) / (3*256);
  const float dontRoundAbove = 220;
  float roundUpR, roundUpG, roundUpB;
  
  if (usegammaOnLastPaletteGeneration != usegamma) {
    if (Palettes15) free(Palettes15);
    if (Palettes16) free(Palettes16);
    if (Palettes32) free(Palettes32);
    Palettes15 = NULL;
    Palettes16 = NULL;
    Palettes32 = NULL;
    usegammaOnLastPaletteGeneration = usegamma;      
  }
  
  if (mode == VID_MODE32) {
    if (!Palettes32) {
      // set int palette
      Palettes32 = malloc(numPals*256*sizeof(int)*VID_NUMCOLORWEIGHTS);
      for (p=0; p<numPals; p++) {
        for (i=0; i<256; i++) {
          r = gtable[pal[(256*p+i)*3+0]];
          g = gtable[pal[(256*p+i)*3+1]];
          b = gtable[pal[(256*p+i)*3+2]];
          
          // ideally, we should always round up, but very bright colors
          // overflow the blending adds, so they don't get rounded.
          roundUpR = (r > dontRoundAbove) ? 0 : 0.5f;
          roundUpG = (g > dontRoundAbove) ? 0 : 0.5f;
          roundUpB = (b > dontRoundAbove) ? 0 : 0.5f;
                  
          for (w=0; w<VID_NUMCOLORWEIGHTS; w++) {
            t = (float)(w)/(float)(VID_NUMCOLORWEIGHTS-1);
            nr = (int)(r*t+roundUpR);
            ng = (int)(g*t+roundUpG);
            nb = (int)(b*t+roundUpB);
            Palettes32[((p*256+i)*VID_NUMCOLORWEIGHTS)+w] = (
              (nr<<16) | (ng<<8) | nb
            );
          }
        }
      }
    }
    V_Palette32 = Palettes32 + paletteNum*256*VID_NUMCOLORWEIGHTS;
  }
  else if (mode == VID_MODE16) {
    if (!Palettes16) {
      // set short palette
      Palettes16 = malloc(numPals*256*sizeof(short)*VID_NUMCOLORWEIGHTS);
      for (p=0; p<numPals; p++) {
        for (i=0; i<256; i++) {
          r = gtable[pal[(256*p+i)*3+0]];
          g = gtable[pal[(256*p+i)*3+1]];
          b = gtable[pal[(256*p+i)*3+2]];
          
          // ideally, we should always round up, but very bright colors
          // overflow the blending adds, so they don't get rounded.
          roundUpR = (r > dontRoundAbove) ? 0 : 0.5f;
          roundUpG = (g > dontRoundAbove) ? 0 : 0.5f;
          roundUpB = (b > dontRoundAbove) ? 0 : 0.5f;
                   
          for (w=0; w<VID_NUMCOLORWEIGHTS; w++) {
            t = (float)(w)/(float)(VID_NUMCOLORWEIGHTS-1);
            nr = (int)((r>>3)*t+roundUpR);
            ng = (int)((g>>2)*t+roundUpG);
            nb = (int)((b>>3)*t+roundUpB);
            Palettes16[((p*256+i)*VID_NUMCOLORWEIGHTS)+w] = (
              (nr<<11) | (ng<<5) | nb
            );
          }
        }
      }
    }
    V_Palette16 = Palettes16 + paletteNum*256*VID_NUMCOLORWEIGHTS;
  }
  else if (mode == VID_MODE15) {
    if (!Palettes15) {
      // set short palette
      Palettes15 = malloc(numPals*256*sizeof(short)*VID_NUMCOLORWEIGHTS);
      for (p=0; p<numPals; p++) {
        for (i=0; i<256; i++) {
          r = gtable[pal[(256*p+i)*3+0]];
          g = gtable[pal[(256*p+i)*3+1]];
          b = gtable[pal[(256*p+i)*3+2]];
          
          // ideally, we should always round up, but very bright colors
          // overflow the blending adds, so they don't get rounded.
          roundUpR = (r > dontRoundAbove) ? 0 : 0.5f;
          roundUpG = (g > dontRoundAbove) ? 0 : 0.5f;
          roundUpB = (b > dontRoundAbove) ? 0 : 0.5f;
                   
          for (w=0; w<VID_NUMCOLORWEIGHTS; w++) {
            t = (float)(w)/(float)(VID_NUMCOLORWEIGHTS-1);
            nr = (int)((r>>3)*t+roundUpR);
            ng = (int)((g>>3)*t+roundUpG);
            nb = (int)((b>>3)*t+roundUpB);
            Palettes15[((p*256+i)*VID_NUMCOLORWEIGHTS)+w] = (
              (nr<<10) | (ng<<5) | nb
            );
          }
        }
      }
    }
    V_Palette15 = Palettes15 + paletteNum*256*VID_NUMCOLORWEIGHTS;
  }       
   
  W_UnlockLumpNum(pplump);
  W_UnlockLumpNum(gtlump);
}


//---------------------------------------------------------------------------
// V_DestroyTrueColorPalette
//---------------------------------------------------------------------------
static void V_DestroyTrueColorPalette(video_mode_t mode) {
  if (mode == VID_MODE15) {
    if (Palettes15) free(Palettes15);
    Palettes15 = NULL;
    V_Palette15 = NULL;
  }
  if (mode == VID_MODE16) {
    if (Palettes16) free(Palettes16);
    Palettes16 = NULL;
    V_Palette16 = NULL;
  }
  if (mode == VID_MODE32) {
    if (Palettes32) free(Palettes32);
    Palettes32 = NULL;
    V_Palette32 = NULL;
  }
}

void V_DestroyUnusedTrueColorPalettes(void) {
  if (V_GetMode() != VID_MODE15) V_DestroyTrueColorPalette(VID_MODE15);
  if (V_GetMode() != VID_MODE16) V_DestroyTrueColorPalette(VID_MODE16);
  if (V_GetMode() != VID_MODE32) V_DestroyTrueColorPalette(VID_MODE32);  
}

//
// V_SetPalette
//
// CPhipps - New function to set the palette to palette number pal.
// Handles loading of PLAYPAL and calls I_SetPalette

void V_SetPalette(int pal)
{
  currentPaletteIndex = pal;

  if (V_GetMode() == VID_MODEGL) {
#ifdef GL_DOOM
    gld_SetPalette(pal);
#endif
  } else {
    I_SetPalette(pal);
    if (V_GetMode() == VID_MODE15 || V_GetMode() == VID_MODE16 || V_GetMode() == VID_MODE32) {
      // V_SetPalette can be called as part of the gamma setting before
      // we've loaded any wads, which prevents us from reading the palette - POPE
      if (W_CheckNumForName("PLAYPAL") >= 0) {
        V_UpdateTrueColorPalette(V_GetMode());
      }
    }
  }
}

//
// V_FillRect
//
// CPhipps - New function to fill a rectangle with a given colour
static void V_FillRect8(int scrn, int x, int y, int width, int height, byte colour)
{
  byte* dest = screens[scrn].data + x + y*screens[scrn].byte_pitch;
  while (height--) {
    memset(dest, colour, width);
    dest += screens[scrn].byte_pitch;
  }
}

static void V_FillRect15(int scrn, int x, int y, int width, int height, byte colour)
{
  unsigned short* dest = (unsigned short *)screens[scrn].data + x + y*screens[scrn].short_pitch;
  int w;
  short c = VID_PAL15(colour, VID_COLORWEIGHTMASK);
  while (height--) {
    for (w=0; w<width; w++) {
      dest[w] = c;
    }
    dest += screens[scrn].short_pitch;
  }
}

static void V_FillRect16(int scrn, int x, int y, int width, int height, byte colour)
{
  unsigned short* dest = (unsigned short *)screens[scrn].data + x + y*screens[scrn].short_pitch;
  int w;
  short c = VID_PAL16(colour, VID_COLORWEIGHTMASK);
  while (height--) {
    for (w=0; w<width; w++) {
      dest[w] = c;
    }
    dest += screens[scrn].short_pitch;
  }
}

static void V_FillRect32(int scrn, int x, int y, int width, int height, byte colour)
{
  unsigned int* dest = (unsigned int *)screens[scrn].data + x + y*screens[scrn].int_pitch;
  int w;
  int c = VID_PAL32(colour, VID_COLORWEIGHTMASK);
  while (height--) {
    for (w=0; w<width; w++) {
      dest[w] = c;
    }
    dest += screens[scrn].int_pitch;
  }
}

static void WRAP_V_DrawLine(fline_t* fl, int color);
static void V_PlotPixel8(int scrn, int x, int y, byte color);
static void V_PlotPixel15(int scrn, int x, int y, byte color);
static void V_PlotPixel16(int scrn, int x, int y, byte color);
static void V_PlotPixel32(int scrn, int x, int y, byte color);

#ifdef GL_DOOM
static void WRAP_gld_FillRect(int scrn, int x, int y, int width, int height, byte colour)
{
  gld_FillBlock(x,y,width,height,colour);
}
static void WRAP_gld_CopyRect(int srcx, int srcy, int srcscrn, int width, int height, int destx, int desty, int destscrn, enum patch_translation_e flags)
{
}
static void WRAP_gld_DrawBackground(const char *flatname, int n)
{
  gld_FillFlatName(flatname, 0, 0, SCREENWIDTH, SCREENHEIGHT, VPT_NONE);
}
static void WRAP_gld_FillFlat(int lump, int n, int x, int y, int width, int height, enum patch_translation_e flags)
{
  gld_FillFlat(lump, x, y, width, height, flags);
}
static void WRAP_gld_FillPatch(int lump, int n, int x, int y, int width, int height, enum patch_translation_e flags)
{
  gld_FillPatch(lump, x, y, width, height, flags);
}
static void WRAP_gld_DrawNumPatch(int x, int y, int scrn, int lump, int cm, enum patch_translation_e flags)
{
  gld_DrawNumPatch(x,y,lump,cm,flags);
}
static void WRAP_gld_DrawBlock(int x, int y, int scrn, int width, int height, const byte *src, enum patch_translation_e flags)
{
}
static void V_PlotPixelGL(int scrn, int x, int y, byte color) {
  gld_DrawLine(x-1, y, x+1, y, color);
  gld_DrawLine(x, y-1, x, y+1, color);
}
static void WRAP_gld_DrawLine(fline_t* fl, int color)
{
  gld_DrawLine(fl->a.x, fl->a.y, fl->b.x, fl->b.y, color);
}
#endif

static void NULL_FillRect(int scrn, int x, int y, int width, int height, byte colour) {}
static void NULL_CopyRect(int srcx, int srcy, int srcscrn, int width, int height, int destx, int desty, int destscrn, enum patch_translation_e flags) {}
static void NULL_FillFlat(int lump, int n, int x, int y, int width, int height, enum patch_translation_e flags) {}
static void NULL_FillPatch(int lump, int n, int x, int y, int width, int height, enum patch_translation_e flags) {}
static void NULL_DrawBackground(const char *flatname, int n) {}
static void NULL_DrawNumPatch(int x, int y, int scrn, int lump, int cm, enum patch_translation_e flags) {}
static void NULL_DrawBlock(int x, int y, int scrn, int width, int height, const byte *src, enum patch_translation_e flags) {}
static void NULL_PlotPixel(int scrn, int x, int y, byte color) {}
static void NULL_DrawLine(fline_t* fl, int color) {}

const char *default_videomode;
static video_mode_t current_videomode = VID_MODE8;

V_CopyRect_f V_CopyRect = NULL_CopyRect;
V_FillRect_f V_FillRect = NULL_FillRect;
V_DrawNumPatch_f V_DrawNumPatch = NULL_DrawNumPatch;
V_FillFlat_f V_FillFlat = NULL_FillFlat;
V_FillPatch_f V_FillPatch = NULL_FillPatch;
V_DrawBackground_f V_DrawBackground = NULL_DrawBackground;
V_PlotPixel_f V_PlotPixel = NULL_PlotPixel;
V_DrawLine_f V_DrawLine = NULL_DrawLine;

//
// V_InitMode
//
void V_InitMode(video_mode_t mode) {
#ifndef GL_DOOM
  if (mode == VID_MODEGL)
    mode = VID_MODE8;
#endif
  switch (mode) {
    case VID_MODE8:
      lprintf(LO_INFO, "V_InitMode: using 8 bit video mode\n");
      V_CopyRect = FUNC_V_CopyRect;
      V_FillRect = V_FillRect8;
      V_DrawNumPatch = FUNC_V_DrawNumPatch;
      V_FillFlat = FUNC_V_FillFlat;
      V_FillPatch = FUNC_V_FillPatch;
      V_DrawBackground = FUNC_V_DrawBackground;
      V_PlotPixel = V_PlotPixel8;
      V_DrawLine = WRAP_V_DrawLine;
      current_videomode = VID_MODE8;
      break;
    case VID_MODE15:
      lprintf(LO_INFO, "V_InitMode: using 15 bit video mode\n");
      V_CopyRect = FUNC_V_CopyRect;
      V_FillRect = V_FillRect15;
      V_DrawNumPatch = FUNC_V_DrawNumPatch;
      V_FillFlat = FUNC_V_FillFlat;
      V_FillPatch = FUNC_V_FillPatch;
      V_DrawBackground = FUNC_V_DrawBackground;
      V_PlotPixel = V_PlotPixel15;
      V_DrawLine = WRAP_V_DrawLine;
      current_videomode = VID_MODE15;
      break;
    case VID_MODE16:
      lprintf(LO_INFO, "V_InitMode: using 16 bit video mode\n");
      V_CopyRect = FUNC_V_CopyRect;
      V_FillRect = V_FillRect16;
      V_DrawNumPatch = FUNC_V_DrawNumPatch;
      V_FillFlat = FUNC_V_FillFlat;
      V_FillPatch = FUNC_V_FillPatch;
      V_DrawBackground = FUNC_V_DrawBackground;
      V_PlotPixel = V_PlotPixel16;
      V_DrawLine = WRAP_V_DrawLine;
      current_videomode = VID_MODE16;
      break;
    case VID_MODE32:
      lprintf(LO_INFO, "V_InitMode: using 32 bit video mode\n");
      V_CopyRect = FUNC_V_CopyRect;
      V_FillRect = V_FillRect32;
      V_DrawNumPatch = FUNC_V_DrawNumPatch;
      V_FillFlat = FUNC_V_FillFlat;
      V_FillPatch = FUNC_V_FillPatch;
      V_DrawBackground = FUNC_V_DrawBackground;
      V_PlotPixel = V_PlotPixel32;
      V_DrawLine = WRAP_V_DrawLine;
      current_videomode = VID_MODE32;
      break;
#ifdef GL_DOOM
    case VID_MODEGL:
      lprintf(LO_INFO, "V_InitMode: using OpenGL video mode\n");
      V_CopyRect = WRAP_gld_CopyRect;
      V_FillRect = WRAP_gld_FillRect;
      V_DrawNumPatch = WRAP_gld_DrawNumPatch;
      V_FillFlat = WRAP_gld_FillFlat;
      V_FillPatch = WRAP_gld_FillPatch;
      V_DrawBackground = WRAP_gld_DrawBackground;
      V_PlotPixel = V_PlotPixelGL;
      V_DrawLine = WRAP_gld_DrawLine;
      current_videomode = VID_MODEGL;
      break;
#endif
  }
  R_FilterInit();
}

//
// V_GetMode
//
video_mode_t V_GetMode(void) {
  return current_videomode;
}

//
// V_GetModePixelDepth
//
int V_GetModePixelDepth(video_mode_t mode) {
  switch (mode) {
    case VID_MODE8: return 1;
    case VID_MODE15: return 2;
    case VID_MODE16: return 2;
    case VID_MODE32: return 4;
    default: return 0;
  }
}

//
// V_GetNumPixelBits
//
int V_GetNumPixelBits(void) {
  switch (current_videomode) {
    case VID_MODE8: return 8;
    case VID_MODE15: return 15;
    case VID_MODE16: return 16;
    case VID_MODE32: return 32;
    default: return 0;
  }
}

//
// V_GetPixelDepth
//
int V_GetPixelDepth(void) {
  return V_GetModePixelDepth(current_videomode);
}

//
// V_AllocScreen
//
void V_AllocScreen(screeninfo_t *scrn) {
  if (!scrn->not_on_heap)
    if ((scrn->byte_pitch * scrn->height) > 0)
      //e6y: Clear the screen to black.
      scrn->data = calloc(scrn->byte_pitch*scrn->height, 1);
}

//
// V_AllocScreens
//
void V_AllocScreens(void) {
  int i;

  for (i=0; i<NUM_SCREENS; i++)
    V_AllocScreen(&screens[i]);
}

//
// V_FreeScreen
//
void V_FreeScreen(screeninfo_t *scrn) {
  if (!scrn->not_on_heap) {
    free(scrn->data);
    scrn->data = NULL;
  }
}

//
// V_FreeScreens
//
void V_FreeScreens(void) {
  int i;

  for (i=0; i<NUM_SCREENS; i++)
    V_FreeScreen(&screens[i]);
}

static void V_PlotPixel8(int scrn, int x, int y, byte color) {
  screens[scrn].data[x+screens[scrn].byte_pitch*y] = color;
}

static void V_PlotPixel15(int scrn, int x, int y, byte color) {
  ((unsigned short *)screens[scrn].data)[x+screens[scrn].short_pitch*y] = VID_PAL15(color, VID_COLORWEIGHTMASK);
}

static void V_PlotPixel16(int scrn, int x, int y, byte color) {
  ((unsigned short *)screens[scrn].data)[x+screens[scrn].short_pitch*y] = VID_PAL16(color, VID_COLORWEIGHTMASK);
}

static void V_PlotPixel32(int scrn, int x, int y, byte color) {
  ((unsigned int *)screens[scrn].data)[x+screens[scrn].int_pitch*y] = VID_PAL32(color, VID_COLORWEIGHTMASK);
}

//
// WRAP_V_DrawLine()
//
// Draw a line in the frame buffer.
// Classic Bresenham w/ whatever optimizations needed for speed
//
// Passed the frame coordinates of line, and the color to be drawn
// Returns nothing
//
static void WRAP_V_DrawLine(fline_t* fl, int color)
{
  register int x;
  register int y;
  register int dx;
  register int dy;
  register int sx;
  register int sy;
  register int ax;
  register int ay;
  register int d;

#ifdef RANGECHECK         // killough 2/22/98
  static int fuck = 0;

  // For debugging only
  if
  (
       fl->a.x < 0 || fl->a.x >= SCREENWIDTH
    || fl->a.y < 0 || fl->a.y >= SCREENHEIGHT
    || fl->b.x < 0 || fl->b.x >= SCREENWIDTH
    || fl->b.y < 0 || fl->b.y >= SCREENHEIGHT
  )
  {
    //jff 8/3/98 use logical output routine
    lprintf(LO_DEBUG, "fuck %d \r", fuck++);
    return;
  }
#endif

#define PUTDOT(xx,yy,cc) V_PlotPixel(0,xx,yy,(byte)cc)

  dx = fl->b.x - fl->a.x;
  ax = 2 * (dx<0 ? -dx : dx);
  sx = dx<0 ? -1 : 1;

  dy = fl->b.y - fl->a.y;
  ay = 2 * (dy<0 ? -dy : dy);
  sy = dy<0 ? -1 : 1;

  x = fl->a.x;
  y = fl->a.y;

  if (ax > ay)
  {
    d = ay - ax/2;
    while (1)
    {
      PUTDOT(x,y,color);
      if (x == fl->b.x) return;
      if (d>=0)
      {
        y += sy;
        d -= ax;
      }
      x += sx;
      d += ay;
    }
  }
  else
  {
    d = ax - ay/2;
    while (1)
    {
      PUTDOT(x, y, color);
      if (y == fl->b.y) return;
      if (d >= 0)
      {
        x += sx;
        d -= ay;
      }
      y += sy;
      d += ax;
    }
  }
}

static unsigned char *playpal_data = NULL;
const unsigned char* V_GetPlaypal(void)
{
  if (!playpal_data)
  {
    int lump = W_GetNumForName("PLAYPAL");
    int len = W_LumpLength(lump);
    const byte *data = W_CacheLumpNum(lump);
    playpal_data = malloc(len);
    memcpy(playpal_data, data, len);
    W_UnlockLumpNum(lump);
  }

  return playpal_data;
}

void V_FreePlaypal(void)
{
  if (playpal_data)
  {
    free(playpal_data);
    playpal_data = NULL;
  }
}

void V_FillBorder(int lump, byte color)
{
  int bordtop, bordbottom, bordleft, bordright, bord;
  int Width = SCREENWIDTH;
  int Height = SCREENHEIGHT;

  // This is a 4:3 display, so no border to show
  if (wide_ratio == 0)
    return;

  if (render_stretch_hud == patch_stretch_full)
    return;

  if (wide_ratio & 4)
  {
    // Screen is taller than it is wide
    bordleft = bordright = 0;
    bord = Height - Height * BaseRatioSizes[wide_ratio].multiplier / 48;
    bordtop = bord / 2;
    bordbottom = bord - bordtop;
  }
  else
  {
    // Screen is wider than it is tall
    bordtop = bordbottom = 0;
    bord = Width - Width * BaseRatioSizes[wide_ratio].multiplier / 48;
    bordleft = bord / 2;
    bordright = bord - bordleft;
  }

  if (lump >= 0)
  {
    // Top
    V_FillFlat(lump, 0, 0, 0, Width, bordtop, VPT_NONE);
    // Left
    V_FillFlat(lump, 0, 0, bordtop, bordleft, Height - bordbottom - bordtop, VPT_NONE);
    // Right
    V_FillFlat(lump, 0, Width - bordright, bordtop, bordright, Height - bordbottom - bordtop, VPT_NONE);
    // Bottom
    V_FillFlat(lump, 0, 0, Width - bordbottom, Width, bordbottom, VPT_NONE);
  }
  else
  {
    // Top
    V_FillRect(0, 0, 0, Width, bordtop, color);
    // Left
    V_FillRect(0, 0, bordtop, bordleft, Height - bordbottom - bordtop, color);
    // Right
    V_FillRect(0, Width - bordright, bordtop, bordright, Height - bordbottom - bordtop, color);
    // Bottom
    V_FillRect(0, 0, Width - bordbottom, Width, bordbottom, color);
  }
}

// Tries to guess the physical dimensions of the screen based on the
// screen's pixel dimensions. Can return:
// 0: 4:3
// 1: 16:9
// 2: 16:10
// 4: 5:4
void CheckRatio (int width, int height)
{
  // Assume anything else is 4:3.
  wide_ratio = 0;

  if ((render_aspect >= 1) && (render_aspect <= 4))
  {
    // [SP] User wants to force aspect ratio; let them.
    wide_ratio = (render_aspect == 3 ? 0 : (int)render_aspect);
  }
  else
  {
    // If the size is approximately 16:9, consider it so.
    if (abs (height * 16/9 - width) < 10)
    {
      wide_ratio = 1;
    }
    else
    {
      // 16:10 has more variance in the pixel dimensions. Grr.
      if (abs (height * 16/10 - width) < 60)
      {
        // 320x200 and 640x400 are always 4:3, not 16:10
        if ((width == 320 && height == 200) || (width == 640 && height == 400))
        {
          wide_ratio = 0;
        }
        else
        {
          wide_ratio = 2;
        }
      }
    }
  }

  if (wide_ratio & 4)
  {
    WIDE_SCREENWIDTH = SCREENWIDTH;
  }
  else
  {
    WIDE_SCREENWIDTH = SCREENWIDTH * BaseRatioSizes[wide_ratio].multiplier / 48;
  }

  ST_SCALED_HEIGHT = ST_HEIGHT;
  ST_SCALED_WIDTH  = ST_WIDTH;

  while (ST_SCALED_WIDTH * 2 <= SCREENWIDTH && ST_SCALED_HEIGHT * 2 <= SCREENHEIGHT)
  {
    ST_SCALED_HEIGHT <<= 1;
    ST_SCALED_WIDTH <<= 1;
  }

  patches_scalex = ST_SCALED_HEIGHT / ST_HEIGHT;
  patches_scaley = ST_SCALED_WIDTH / ST_WIDTH;

  switch (render_stretch_hud)
  {
  case patch_stretch_16x10:
    ST_SCALED_Y = (200 * patches_scaley - ST_SCALED_HEIGHT);

    wide_offsetx = (SCREENWIDTH - patches_scalex * 320) / 2;
    wide_offsety = (SCREENHEIGHT - patches_scaley * 200) / 2;
    break;
  case patch_stretch_4x3:
    ST_SCALED_HEIGHT = ST_HEIGHT * SCREENHEIGHT / 200;
    ST_SCALED_WIDTH  = WIDE_SCREENWIDTH;

    ST_SCALED_Y = SCREENHEIGHT - ST_SCALED_HEIGHT;
    wide_offsetx = (SCREENWIDTH - WIDE_SCREENWIDTH) / 2;
    wide_offsety = 0;
    break;
  case patch_stretch_full:
    ST_SCALED_HEIGHT = ST_HEIGHT * SCREENHEIGHT / 200;
    ST_SCALED_WIDTH  = SCREENWIDTH;

    ST_SCALED_Y = SCREENHEIGHT - ST_SCALED_HEIGHT;
    wide_offsetx = 0;
    wide_offsety = 0;
    break;
  }
}
