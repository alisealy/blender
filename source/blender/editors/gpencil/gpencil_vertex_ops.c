/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2015, Blender Foundation
 * This is a new part of Blender
 * Brush based operators for editing Grease Pencil strokes
 */

/** \file
 * \ingroup edgpencil
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_ghash.h"
#include "BLI_math.h"

#include "BLT_translation.h"

#include "DNA_brush_types.h"
#include "DNA_gpencil_types.h"

#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_paint.h"
#include "BKE_report.h"

#include "WM_api.h"
#include "WM_types.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_enum_types.h"

#include "UI_view2d.h"

#include "ED_gpencil.h"
#include "ED_screen.h"
#include "ED_view3d.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "gpencil_intern.h"

enum {
  GP_PAINT_VERTEX_STROKE = 0,
  GP_PAINT_VERTEX_FILL = 1,
  GP_PAINT_VERTEX_BOTH = 2,
};

static const EnumPropertyItem gpencil_modesEnumPropertyItem_mode[] = {
    {GP_PAINT_VERTEX_STROKE, "STROKE", 0, "Stroke", ""},
    {GP_PAINT_VERTEX_FILL, "FILL", 0, "Fill", ""},
    {GP_PAINT_VERTEX_BOTH, "BOTH", 0, "Both", ""},
    {0, NULL, 0, NULL, NULL},
};

/* Poll callback for stroke vertex paint operator. */
static bool gp_vertexpaint_mode_poll(bContext *C)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  Object *ob = CTX_data_active_object(C);
  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
    return false;
  }

  bGPdata *gpd = (bGPdata *)ob->data;
  if (GPENCIL_VERTEX_MODE(gpd)) {
    if (!(GPENCIL_ANY_VERTEX_MASK(ts->gpencil_selectmode_vertex))) {
      return false;
    }

    /* Any data to use. */
    if (gpd->layers.first) {
      return true;
    }
  }

  return false;
}

static int gp_vertexpaint_brightness_contrast_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = (bGPdata *)ob->data;
  bool changed = false;
  int i;
  bGPDspoint *pt;
  const int mode = RNA_enum_get(op->ptr, "mode");

  float gain, offset;
  {
    float brightness = RNA_float_get(op->ptr, "brightness");
    float contrast = RNA_float_get(op->ptr, "contrast");
    brightness /= 100.0f;
    float delta = contrast / 200.0f;
    /*
     * The algorithm is by Werner D. Streidt
     * (http://visca.com/ffactory/archives/5-99/msg00021.html)
     * Extracted of OpenCV demhist.c
     */
    if (contrast > 0) {
      gain = 1.0f - delta * 2.0f;
      gain = 1.0f / max_ff(gain, FLT_EPSILON);
      offset = gain * (brightness - delta);
    }
    else {
      delta *= -1;
      gain = max_ff(1.0f - delta * 2.0f, 0.0f);
      offset = gain * brightness + delta;
    }
  }

  /* Loop all selected strokes. */
  GP_EDITABLE_STROKES_BEGIN (gpstroke_iter, C, gpl, gps) {
    if (gps->flag & GP_STROKE_SELECT) {
      changed = true;
      /* Fill color. */
      if (gps->flag & GP_STROKE_SELECT) {
        changed = true;
        if (mode != GP_PAINT_VERTEX_STROKE) {
          if (gps->mix_color_fill[3] > 0.0f) {
            for (int i = 0; i < 3; i++) {
              gps->mix_color_fill[i] = gain * gps->mix_color_fill[i] + offset;
            }
          }
        }
      }

      /* Stroke points. */
      if (mode != GP_PAINT_VERTEX_FILL) {
        for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
          if ((pt->flag & GP_SPOINT_SELECT) && (pt->mix_color[3] > 0.0f)) {
            for (int i = 0; i < 3; i++) {
              pt->mix_color[i] = gain * pt->mix_color[i] + offset;
            }
          }
        }
      }
    }
  }
  GP_EDITABLE_STROKES_END(gpstroke_iter);

  /* notifiers */
  if (changed) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_vertex_color_brightness_contrast(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Vertex Paint Bright/Contrast";
  ot->idname = "GPENCIL_OT_vertex_color_brightness_contrast";
  ot->description = "Adjust vertex color brightness/contrast";

  /* api callbacks */
  ot->exec = gp_vertexpaint_brightness_contrast_exec;
  ot->poll = gp_vertexpaint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* params */
  ot->prop = RNA_def_enum(ot->srna, "mode", gpencil_modesEnumPropertyItem_mode, 0, "Mode", "");
  const float min = -100, max = +100;
  prop = RNA_def_float(ot->srna, "brightness", 0.0f, min, max, "Brightness", "", min, max);
  prop = RNA_def_float(ot->srna, "contrast", 0.0f, min, max, "Contrast", "", min, max);
  RNA_def_property_ui_range(prop, min, max, 1, 1);
}

static int gp_vertexpaint_hsv_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = (bGPdata *)ob->data;

  bool changed = false;
  int i;
  bGPDspoint *pt;
  float hsv[3];

  const int mode = RNA_enum_get(op->ptr, "mode");
  float hue = RNA_float_get(op->ptr, "h");
  float sat = RNA_float_get(op->ptr, "s");
  float val = RNA_float_get(op->ptr, "v");

  /* Loop all selected strokes. */
  GP_EDITABLE_STROKES_BEGIN (gpstroke_iter, C, gpl, gps) {
    if (gps->flag & GP_STROKE_SELECT) {
      changed = true;

      /* Fill color. */
      if (mode != GP_PAINT_VERTEX_STROKE) {
        if (gps->mix_color_fill[3] > 0.0f) {

          rgb_to_hsv_v(gps->mix_color_fill, hsv);

          hsv[0] += (hue - 0.5f);
          if (hsv[0] > 1.0f) {
            hsv[0] -= 1.0f;
          }
          else if (hsv[0] < 0.0f) {
            hsv[0] += 1.0f;
          }
          hsv[1] *= sat;
          hsv[2] *= val;

          hsv_to_rgb_v(hsv, gps->mix_color_fill);
        }
      }

      /* Stroke points. */
      if (mode != GP_PAINT_VERTEX_FILL) {
        for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
          if ((pt->flag & GP_SPOINT_SELECT) && (pt->mix_color[3] > 0.0f)) {
            rgb_to_hsv_v(pt->mix_color, hsv);

            hsv[0] += (hue - 0.5f);
            if (hsv[0] > 1.0f) {
              hsv[0] -= 1.0f;
            }
            else if (hsv[0] < 0.0f) {
              hsv[0] += 1.0f;
            }
            hsv[1] *= sat;
            hsv[2] *= val;

            hsv_to_rgb_v(hsv, pt->mix_color);
          }
        }
      }
    }
  }
  GP_EDITABLE_STROKES_END(gpstroke_iter);

  /* notifiers */
  if (changed) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_vertex_color_hsv(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Vertex Paint Hue Saturation Value";
  ot->idname = "GPENCIL_OT_vertex_color_hsv";
  ot->description = "Adjust vertex color HSV values";

  /* api callbacks */
  ot->exec = gp_vertexpaint_hsv_exec;
  ot->poll = gp_vertexpaint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* params */
  ot->prop = RNA_def_enum(ot->srna, "mode", gpencil_modesEnumPropertyItem_mode, 0, "Mode", "");
  RNA_def_float(ot->srna, "h", 0.5f, 0.0f, 1.0f, "Hue", "", 0.0f, 1.0f);
  RNA_def_float(ot->srna, "s", 1.0f, 0.0f, 2.0f, "Saturation", "", 0.0f, 2.0f);
  RNA_def_float(ot->srna, "v", 1.0f, 0.0f, 2.0f, "Value", "", 0.0f, 2.0f);
}

static int gp_vertexpaint_invert_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = (bGPdata *)ob->data;

  bool changed = false;
  int i;
  bGPDspoint *pt;

  const int mode = RNA_enum_get(op->ptr, "mode");

  /* Loop all selected strokes. */
  GP_EDITABLE_STROKES_BEGIN (gpstroke_iter, C, gpl, gps) {
    if (gps->flag & GP_STROKE_SELECT) {
      changed = true;
      /* Fill color. */
      if (gps->flag & GP_STROKE_SELECT) {
        changed = true;
        if (mode != GP_PAINT_VERTEX_STROKE) {
          if (gps->mix_color_fill[3] > 0.0f) {
            for (int i = 0; i < 3; i++) {
              gps->mix_color_fill[i] = 1.0f - gps->mix_color_fill[i];
            }
          }
        }
      }

      /* Stroke points. */
      if (mode != GP_PAINT_VERTEX_FILL) {
        for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
          if ((pt->flag & GP_SPOINT_SELECT) && (pt->mix_color[3] > 0.0f)) {
            for (int i = 0; i < 3; i++) {
              pt->mix_color[i] = 1.0f - pt->mix_color[i];
            }
          }
        }
      }
    }
  }
  GP_EDITABLE_STROKES_END(gpstroke_iter);

  /* notifiers */
  if (changed) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_vertex_color_invert(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Vertex Paint Invert";
  ot->idname = "GPENCIL_OT_vertex_color_invert";
  ot->description = "Invert RGB values";

  /* api callbacks */
  ot->exec = gp_vertexpaint_invert_exec;
  ot->poll = gp_vertexpaint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* params */
  ot->prop = RNA_def_enum(ot->srna, "mode", gpencil_modesEnumPropertyItem_mode, 0, "Mode", "");
}

static int gp_vertexpaint_levels_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = (bGPdata *)ob->data;

  bool changed = false;
  int i;
  bGPDspoint *pt;

  const int mode = RNA_enum_get(op->ptr, "mode");
  float gain = RNA_float_get(op->ptr, "gain");
  float offset = RNA_float_get(op->ptr, "offset");

  /* Loop all selected strokes. */
  GP_EDITABLE_STROKES_BEGIN (gpstroke_iter, C, gpl, gps) {

    /* Fill color. */
    if (gps->flag & GP_STROKE_SELECT) {
      changed = true;
      if (mode != GP_PAINT_VERTEX_STROKE) {
        if (gps->mix_color_fill[3] > 0.0f) {
          for (int i = 0; i < 3; i++) {
            gps->mix_color_fill[i] = gain * (gps->mix_color_fill[i] + offset);
          }
        }
      }
    }

    /* Stroke points. */
    if (mode != GP_PAINT_VERTEX_FILL) {
      for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
        if ((pt->flag & GP_SPOINT_SELECT) && (pt->mix_color[3] > 0.0f)) {
          for (int i = 0; i < 3; i++) {
            pt->mix_color[i] = gain * (pt->mix_color[i] + offset);
          }
        }
      }
    }
  }
  GP_EDITABLE_STROKES_END(gpstroke_iter);

  /* notifiers */
  if (changed) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_vertex_color_levels(wmOperatorType *ot)
{

  /* identifiers */
  ot->name = "Vertex Paint Levels";
  ot->idname = "GPENCIL_OT_vertex_color_levels";
  ot->description = "Adjust levels of vertex colors";

  /* api callbacks */
  ot->exec = gp_vertexpaint_levels_exec;
  ot->poll = gp_vertexpaint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* params */
  ot->prop = RNA_def_enum(ot->srna, "mode", gpencil_modesEnumPropertyItem_mode, 0, "Mode", "");

  RNA_def_float(
      ot->srna, "offset", 0.0f, -1.0f, 1.0f, "Offset", "Value to add to colors", -1.0f, 1.0f);
  RNA_def_float(
      ot->srna, "gain", 1.0f, 0.0f, FLT_MAX, "Gain", "Value to multiply colors by", 0.0f, 10.0f);
}

static int gp_vertexpaint_set_exec(bContext *C, wmOperator *op)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = (bGPdata *)ob->data;
  Paint *paint = &ts->gp_vertexpaint->paint;
  Brush *brush = brush = paint->brush;

  bool changed = false;
  int i;
  bGPDspoint *pt;

  const int mode = RNA_enum_get(op->ptr, "mode");
  float factor = RNA_float_get(op->ptr, "factor");

  /* Loop all selected strokes. */
  GP_EDITABLE_STROKES_BEGIN (gpstroke_iter, C, gpl, gps) {

    /* Fill color. */
    if (gps->flag & GP_STROKE_SELECT) {
      changed = true;
      if (mode != GP_PAINT_VERTEX_STROKE) {
        copy_v3_v3(gps->mix_color_fill, brush->rgb);
        gps->mix_color_fill[3] = factor;
      }
    }

    /* Stroke points. */
    if (mode != GP_PAINT_VERTEX_FILL) {
      for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
        if (pt->flag & GP_SPOINT_SELECT) {
          copy_v3_v3(pt->mix_color, brush->rgb);
          pt->mix_color[3] = factor;
        }
      }
    }
  }
  GP_EDITABLE_STROKES_END(gpstroke_iter);

  /* notifiers */
  if (changed) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_vertex_color_set(wmOperatorType *ot)
{

  /* identifiers */
  ot->name = "Vertex Paint Set Color";
  ot->idname = "GPENCIL_OT_vertex_color_set";
  ot->description = "Set active color to all selected vertex";

  /* api callbacks */
  ot->exec = gp_vertexpaint_set_exec;
  ot->poll = gp_vertexpaint_mode_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* params */
  ot->prop = RNA_def_enum(ot->srna, "mode", gpencil_modesEnumPropertyItem_mode, 0, "Mode", "");
  RNA_def_float(ot->srna, "factor", 1.0f, 0.001f, 1.0f, "Factor", "Mix Factor", 0.001f, 1.0f);
}

/* Helper to extract color for palette. */
static bool gp_extract_palette(bContext *C, const bool selected)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);
  bool done = false;

  GHash *color_table = BLI_ghash_int_new(__func__);

  /* Extract all colors. */
  CTX_DATA_BEGIN (C, bGPDlayer *, gpl, editable_gpencil_layers) {
    for (bGPDframe *gpf = gpl->frames.first; gpf; gpf = gpf->next) {
      for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
        if (ED_gpencil_stroke_can_use(C, gps) == false) {
          continue;
        }
        if (ED_gpencil_stroke_color_use(ob, gpl, gps) == false) {
          continue;
        }
        MaterialGPencilStyle *gp_style = BKE_material_gpencil_settings_get(ob, gps->mat_nr + 1);
        if (gp_style == NULL) {
          continue;
        }

        if ((selected) && ((gps->flag & GP_STROKE_SELECT) == 0)) {
          continue;
        }

        bool use_stroke = (gp_style->flag & GP_STYLE_STROKE_SHOW);
        bool use_fill = (gp_style->flag & GP_STYLE_FILL_SHOW);

        /* Material is disabled. */
        if ((!use_fill) && (!use_stroke)) {
          continue;
        }

        /* Only solid strokes or stencil. */
        if ((use_stroke) && ((gp_style->stroke_style == GP_STYLE_STROKE_STYLE_TEXTURE) &&
                             ((gp_style->flag & GP_STYLE_FILL_PATTERN) == 0))) {
          continue;
        }

        /* Only solid fill. */
        if ((use_fill) && (gp_style->fill_style != GP_STYLE_FILL_STYLE_SOLID)) {
          continue;
        }

        /* Fill color. */
        if (gps->mix_color_fill[3] > 0.0f) {
          uint key = rgb_to_cpack(
              gps->mix_color_fill[0], gps->mix_color_fill[1], gps->mix_color_fill[2]);

          if (!BLI_ghash_haskey(color_table, POINTER_FROM_INT(key))) {
            BLI_ghash_insert(color_table, POINTER_FROM_INT(key), POINTER_FROM_INT(key));
          }
        }

        /* Read all points to get all colors. */
        bGPDspoint *pt;
        int i;
        for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
          uint key = rgb_to_cpack(pt->mix_color[0], pt->mix_color[1], pt->mix_color[2]);
          if (!BLI_ghash_haskey(color_table, POINTER_FROM_INT(key))) {
            BLI_ghash_insert(color_table, POINTER_FROM_INT(key), POINTER_FROM_INT(key));
          }
        }
      }
    }
  }
  CTX_DATA_END;

  /* Create the Palette. */
  if (BLI_ghash_len(color_table) > 0) {
    GHashIterator gh_iter;
    Palette *palette = BKE_palette_add(bmain, "Palette");
    if (palette) {
      GHASH_ITER (gh_iter, color_table) {
        const uint col = POINTER_AS_INT(BLI_ghashIterator_getValue(&gh_iter));
        float r, g, b;
        cpack_to_rgb(col, &r, &g, &b);
        PaletteColor *palcol = BKE_palette_color_add(palette);
        if (palcol) {
          palcol->rgb[0] = r;
          palcol->rgb[1] = g;
          palcol->rgb[2] = b;
        }
      }
      done = true;
    }
  }
  else {
    done = false;
  }

  /* Free memory. */
  BLI_ghash_free(color_table, NULL, NULL);

  return done;
}

/* Convert Materials to Vertex Color. */
static bool gp_material_to_vertex_poll(bContext *C)
{
  /* only supported with grease pencil objects */
  Object *ob = CTX_data_active_object(C);
  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
    return false;
  }

  return true;
}

typedef struct GPMatArray {
  uint key;
  Material *ma;
  Image *sima;
  int index;
} GPMatArray;

static uint get_material_type(MaterialGPencilStyle *gp_style,
                              bool use_stroke,
                              bool use_fill,
                              char *name)
{
  uint r_i = -1;
  if ((use_stroke) && (use_fill)) {
    switch (gp_style->mode) {
      case GP_STYLE_MODE_LINE: {
        r_i = 1;
        strcpy(name, "Line Stroke-Fill");
        break;
      }
      case GP_STYLE_MODE_DOTS: {
        r_i = 2;
        strcpy(name, "Dots Stroke-Fill");
        break;
      }
      case GP_STYLE_MODE_BOX: {
        r_i = 3;
        strcpy(name, "Boxes Stroke-Fill");
        break;
      }
      default:
        break;
    }
  }
  else if (use_stroke) {
    switch (gp_style->mode) {
      case GP_STYLE_MODE_LINE: {
        r_i = 4;
        strcpy(name, "Line Stroke");
        break;
      }
      case GP_STYLE_MODE_DOTS: {
        r_i = 5;
        strcpy(name, "Dots Stroke");
        break;
      }
      case GP_STYLE_MODE_BOX: {
        r_i = 6;
        strcpy(name, "Boxes Stroke");
        break;
      }
      default:
        break;
    }
  }
  else {
    r_i = 7;
    strcpy(name, "Solid Fill");
  }

  /* Create key TSSSSFFFF (T: Type S: Stroke Alpha F: Fill Alpha) */
  r_i *= 1e8;
  if (use_stroke) {
    r_i += gp_style->stroke_rgba[3] * 1e7;
  }
  if (use_fill) {
    r_i += gp_style->fill_rgba[3] * 1e3;
  }

  return r_i;
}

static int gp_material_to_vertex_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);
  bGPdata *gpd = (bGPdata *)ob->data;
  const bool remove = RNA_boolean_get(op->ptr, "remove");
  const bool palette = RNA_boolean_get(op->ptr, "palette");
  const bool selected = RNA_boolean_get(op->ptr, "selected");

  char name[32] = "";
  Material *ma = NULL;
  GPMatArray *mat_elm = NULL;

  bool changed = false;

  short *totcol = give_totcolp(ob);
  if (totcol == 0) {
    return OPERATOR_CANCELLED;
  }

  /* These arrays hold all materials and index in the material slots for all combinations. */
  int totmat = *totcol;
  GPMatArray *mat_table = MEM_calloc_arrayN(totmat, sizeof(GPMatArray), __func__);

  /* Update stroke material index. */
  CTX_DATA_BEGIN (C, bGPDlayer *, gpl, editable_gpencil_layers) {
    for (bGPDframe *gpf = gpl->frames.first; gpf; gpf = gpf->next) {
      for (bGPDstroke *gps = gpf->strokes.first; gps; gps = gps->next) {
        if (ED_gpencil_stroke_can_use(C, gps) == false) {
          continue;
        }
        if (ED_gpencil_stroke_color_use(ob, gpl, gps) == false) {
          continue;
        }

        if ((selected) && ((gps->flag & GP_STROKE_SELECT) == 0)) {
          continue;
        }

        MaterialGPencilStyle *gp_style = BKE_material_gpencil_settings_get(ob, gps->mat_nr + 1);
        if (gp_style == NULL) {
          continue;
        }

        bool use_stroke = (gp_style->flag & GP_STYLE_STROKE_SHOW);
        bool use_fill = (gp_style->flag & GP_STYLE_FILL_SHOW);

        /* Material is disabled. */
        if ((!use_fill) && (!use_stroke)) {
          continue;
        }

        /* Only solid strokes or stencil. */
        if ((use_stroke) && ((gp_style->stroke_style == GP_STYLE_STROKE_STYLE_TEXTURE) &&
                             ((gp_style->flag & GP_STYLE_FILL_PATTERN) == 0))) {
          continue;
        }

        /* Only solid fill. */
        if ((use_fill) && (gp_style->fill_style != GP_STYLE_FILL_STYLE_SOLID)) {
          continue;
        }

        /* Create material type unique key by type and alpha. */
        uint key = get_material_type(gp_style, use_stroke, use_fill, name);

        /* Check if material exist. */
        int i;
        bool found = false;
        for (i = 0; i < totmat; i++) {
          mat_elm = &mat_table[i];
          if (mat_elm->ma == NULL) {
            break;
          }
          if (key == mat_elm->key) {
            /* Check if using same stencil texture. */
            if (gp_style->sima == mat_elm->sima) {
              found = true;
              break;
            }
          }
        }

        /* If not found create a new material. */
        if (!found) {
          ma = BKE_material_add_gpencil(bmain, name);
          if (use_stroke) {
            ma->gp_style->flag |= GP_STYLE_STROKE_SHOW;
          }
          else {
            ma->gp_style->flag &= ~GP_STYLE_STROKE_SHOW;
          }

          if (use_fill) {
            ma->gp_style->flag |= GP_STYLE_FILL_SHOW;
          }
          else {
            ma->gp_style->flag &= ~GP_STYLE_FILL_SHOW;
          }

          ma->gp_style->stroke_rgba[3] = gp_style->stroke_rgba[3];
          ma->gp_style->fill_rgba[3] = gp_style->fill_rgba[3];

          BKE_object_material_slot_add(bmain, ob);
          assign_material(bmain, ob, ma, ob->totcol, BKE_MAT_ASSIGN_USERPREF);

          mat_elm->key = key;
          mat_elm->ma = ma;
          mat_elm->sima = gp_style->ima;
          mat_elm->index = ob->totcol - 1;
        }
        else {
          mat_elm = &mat_table[i];
        }

        /* Update stroke */
        changed = true;
        gps->mat_nr = mat_elm->index;
        copy_v3_v3(gps->mix_color_fill, gp_style->fill_rgba);
        gps->mix_color_fill[3] = 1.0f;

        /* Update all points. */
        bGPDspoint *pt;
        for (i = 0, pt = gps->points; i < gps->totpoints; i++, pt++) {
          copy_v3_v3(pt->mix_color, gp_style->stroke_rgba);
          pt->mix_color[3] = 1.0f;
        }
      }
    }
  }
  CTX_DATA_END;

  /* notifiers */
  if (changed) {
    DEG_id_tag_update(&gpd->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, NULL);
  }

  /* Free memory. */
  MEM_SAFE_FREE(mat_table);

  /* Generate a Palette. */
  if (palette) {
    gp_extract_palette(C, selected);
  }

  /* Clean unused materials. */
  if (remove) {
    WM_operator_name_call(
        C, "OBJECT_OT_material_slot_remove_unused", WM_OP_INVOKE_REGION_WIN, NULL);
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_material_to_vertex_color(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Convert Stroke Materials to Vertex Color";
  ot->idname = "GPENCIL_OT_material_to_vertex_color";
  ot->description = "Replace materials in strokes with Vertex Color";

  /* api callbacks */
  ot->exec = gp_material_to_vertex_exec;
  ot->poll = gp_material_to_vertex_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_boolean(ot->srna,
                             "remove",
                             true,
                             "Remove Unused Materiales",
                             "Remove any unused material after the conversion");
  RNA_def_boolean(ot->srna, "palette", true, "Create Palette", "Create a new palette with colors");
  RNA_def_boolean(ot->srna, "selected", false, "Only Selected", "Convert only selected strokes");
}

/* Extract Palette from Vertex Color. */
static bool gp_extract_palette_poll(bContext *C)
{
  /* only supported with grease pencil objects */
  Object *ob = CTX_data_active_object(C);
  if ((ob == NULL) || (ob->type != OB_GPENCIL)) {
    return false;
  }

  return true;
}

static int gp_extract_palette_exec(bContext *C, wmOperator *op)
{
  const bool selected = RNA_boolean_get(op->ptr, "selected");

  if (gp_extract_palette(C, selected)) {
    BKE_reportf(op->reports, RPT_INFO, "Palette created");
  }
  else {
    BKE_reportf(op->reports, RPT_ERROR, "Unable to find Vertex Information to create palette");
  }

  return OPERATOR_FINISHED;
}

void GPENCIL_OT_extract_palette(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Extract Palette from Vertex Color";
  ot->idname = "GPENCIL_OT_extract_palette";
  ot->description = "Extract all colors used in Vertex and create a Palette";

  /* api callbacks */
  ot->exec = gp_extract_palette_exec;
  ot->poll = gp_extract_palette_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_boolean(
      ot->srna, "selected", false, "Only Selected", "Convert only selected strokes");
}
