/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

/*
 * Ventana BigTIFF support
 *
 * quickhash comes from what the TIFF backend does
 *
 */

#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <errno.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

static const char *VENTANA_ISCAN = "/EncodeInfo/SlideInfo/iScan";

struct level {
  int32_t directory;
  int64_t width;
};

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct level *la = a;
  const struct level *lb = b;

  if (la->width > lb->width) {
    return -1;
  } else if (la->width == lb->width) {
    return 0;
  } else {
    return 1;
  }
}

static bool find_property(const char *string_to_parse, const char *prop_name, char **prop_value, bool quotes) {
  bool status = false;
  GRegex *props_regex;
  GMatchInfo *props_match;

  if(quotes) {
    const char *pattern = g_strdup_printf("%s=[\"\'](.*)[\"\']",prop_name);
    props_regex = g_regex_new(pattern, G_REGEX_OPTIMIZE | G_REGEX_UNGREEDY, 0, NULL); // pass err if desired, ignoring for now
  } else {
    const char *pattern = g_strdup_printf("%s=(\\S+)",prop_name);
    props_regex = g_regex_new(pattern, G_REGEX_OPTIMIZE | G_REGEX_UNGREEDY, 0, NULL); // pass err if desired, ignoring for now
  }

  if(!g_regex_match((const GRegex *)props_regex, string_to_parse, 0, &props_match)) {
//    _openslide_set_error(osr, "No properties found");
    goto FAIL;
  }

  *prop_value = g_match_info_fetch(props_match, 0);
  status = true;

FAIL:
  g_match_info_free(props_match);
  g_regex_unref(props_regex);

  return status;
}

// returns NULL if no matches
static xmlXPathObjectPtr eval_xpath(const char *xpath,
                                    xmlXPathContextPtr context) {
  xmlXPathObjectPtr result;

  result = xmlXPathEvalExpression(BAD_CAST xpath, context);
  if (result && (result->nodesetval == NULL ||
                 result->nodesetval->nodeNr == 0)) {
    xmlXPathFreeObject(result);
    result = NULL;
  }
  return result;
}

static void set_prop_from_content(openslide_t *osr,
                                  const char *property_name,
                                  const char *xpath,
                                  xmlXPathContextPtr context) {
  xmlXPathObjectPtr result;

  result = eval_xpath(xpath, context);
  if (result) {
    xmlChar *str = xmlNodeGetContent(result->nodesetval->nodeTab[0]);
    if (osr && str) {
      g_hash_table_insert(osr->properties,
                          g_strdup(property_name),
                          g_strdup((char *) str));
    }
    xmlFree(str);
  }
  xmlXPathFreeObject(result);
}

static void set_prop_from_attribute(openslide_t *osr,
                                    const char *property_name,
                                    const char *xpath,
                                    const char *attribute_name,
                                    xmlXPathContextPtr context) {
  xmlXPathObjectPtr result;

  result = eval_xpath(xpath, context);
  if (result) {
    xmlChar *str = xmlGetProp(result->nodesetval->nodeTab[0],
                              BAD_CAST attribute_name);
    if (osr && str) {
      g_hash_table_insert(osr->properties,
                          g_strdup(property_name),
                          g_strdup((char *) str));
    }
    xmlFree(str);
  }
  xmlXPathFreeObject(result);
}


// add the image from the current TIFF directory
// returns false and sets GError if fatal error
// true does not necessarily imply an image was added
static bool add_associated_image(GHashTable *ht, const char *name_if_available,
				 TIFF *tiff, GError **err) {
  char *name = NULL;
  if (name_if_available) {
    name = g_strdup(name_if_available);
  } else {
    char *val;

    // get name
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &val)) {
      return true;
    }
    name = g_strdup(val);
  }

  if (!name) {
    return true;
  }

  bool result = _openslide_add_tiff_associated_image(ht, name, tiff, err);
  g_free(name);
  return result;
}

static bool parse_xml_description(const char *xml, openslide_t *osr, GError **err) {
/* 
ANB: goals of this function are different from the corresponding leica function (from which this is derived)
1) Image properties are saved in attribues of element 'EncodeInfo/SlideInfo/iScan'
2) Overlap info in 'EncodeInfo/SlideStitchInfo/
*/
  xmlDocPtr doc = NULL;
  xmlNode *iScan;

  xmlXPathContextPtr context = NULL;
  xmlXPathObjectPtr result = NULL;

  bool success = false;

  // try to parse the xml
  doc = xmlReadMemory(xml, strlen(xml), "/", NULL, XML_PARSE_NOERROR |
                      XML_PARSE_NOWARNING | XML_PARSE_NONET);
  if (doc == NULL) {
    // not ventana
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Could not parse XML");
    goto FAIL;
  }

  // create XPATH context to query the document
  context = xmlXPathNewContext(doc);
  if (context == NULL) {
    // allocation error, abort
    g_error("xmlXPathNewContext failed");
    // not reached
  }

  // the recognizable structure is the following:
  /*
    EncodeInfo (root node)
      SlideInfo
        ServerDirectory
	LabelImage
	iScan
	  AOIO
        ...
  */

  result = eval_xpath(VENTANA_ISCAN, context);
  // the root node should only have one child, named iScan, otherwise fail
  if (result == NULL || result->nodesetval->nodeNr != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Multiple iScan elements found");
    goto FAIL;
  }

  iScan = result->nodesetval->nodeTab[0];
  xmlXPathFreeObject(result);
  result = NULL;

  // pull important (i.e. required) properties whose absense would lead to hard failure
  // TODO: fail with ERROR_BAD_DATA if these aren't set
  // read magnification
  set_prop_from_attribute(osr, "ventana.magnification", VENTANA_ISCAN, "Magnification", context);
  set_prop_from_attribute(osr, "ventana.resolution", VENTANA_ISCAN, "ScanRes", context); 

  // add some more (optional) properties from the main image
  set_prop_from_attribute(osr, "ventana.device-model",
                          VENTANA_ISCAN, "UnitNumber",
                          context);
  set_prop_from_attribute(osr, "ventana.build-version",
                          VENTANA_ISCAN, "BuildVersion",
                          context);
  set_prop_from_attribute(osr, "ventana.build-date",
                          VENTANA_ISCAN, "BuildDate",
                          context);
  set_prop_from_attribute(osr, "ventana.slide-annotation",
                          VENTANA_ISCAN, "SlideAnnotation",
                          context);
  set_prop_from_attribute(osr, "ventana.show-label",
                          VENTANA_ISCAN, "ShowLabel",
                          context);
  set_prop_from_attribute(osr, "ventana.label-boundary",
                          VENTANA_ISCAN, "LabelBoundary",
                          context);
  set_prop_from_attribute(osr, "ventana.z-layers",
                          VENTANA_ISCAN, "Z-layers",
                          context);
  set_prop_from_attribute(osr, "ventana.z-spacing",
                          VENTANA_ISCAN, "Z-spacing",
                          context);
  set_prop_from_attribute(osr, "ventana.focus-mode",
                          VENTANA_ISCAN, "FocusMode",
                          context);
  set_prop_from_attribute(osr, "ventana.focus-quality",
                          VENTANA_ISCAN, "FocusQuality",
                          context);
  set_prop_from_attribute(osr, "ventana.scan-mode",
                          VENTANA_ISCAN, "ScanMode",
                          context);
/*  set_prop_from_content(osr, "leica.illumination-source",
                        "l:scanSettings/l:illuminationSettings/l:illuminationSource",
                        context);*/

  // copy magnification and resolution to standard properties
  if (osr) {
    _openslide_duplicate_int_prop(osr->properties, "ventana.magnification",
                                  OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
    _openslide_duplicate_double_prop(osr->properties, "ventana.resolution",
                                     OPENSLIDE_PROPERTY_NAME_MPP_X);
    _openslide_duplicate_double_prop(osr->properties, "ventana.resolution",
                                     OPENSLIDE_PROPERTY_NAME_MPP_Y);  
  }

  success = true;

FAIL:
  xmlXPathFreeObject(result);
  xmlXPathFreeContext(context);
  if (doc != NULL) {
    xmlFreeDoc(doc);
  }

  return success;
}

bool _openslide_try_ventana(openslide_t *osr, TIFF *tiff,
				 struct _openslide_hash *quickhash1,
				 GError **err) {
/* 
NOTE: The following info is drawn from a single slide
1) Associated images:
	a) Label image: first IFD; use "ImageDescription: Label Image"
	b) Macro image: none?
	c) Thumbnail image: second IFD; use "ImageDescription: Thumbnail" 
2) Identify slide as ventana: TIFFTAG_XMLPACKET should be contain "iScan"
3) Properties are stored in TIFFTAG_XMLPACKET instead 
*/

  GList *level_list = NULL;
  int32_t level_count = 0;
  int32_t *levels = NULL;

  // FORMAT_NOT_SUPPORTED if tiff is not tiled or is not identified as belonging to ventana. latter is done at level=0 in do-while loop below.
  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    goto FAIL;
  }

  // if slide is valid and confirmed to be ventana, add vendor name to properties
  if (osr) {
    g_hash_table_insert(osr->properties,
			g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
			g_strdup("ventana"));
  }

  // accumulate tiled levels
  level_count = 0;
  do {
    tdir_t dir = TIFFCurrentDirectory(tiff);

    // confirm that this directory is tiled
    if (!TIFFIsTiled(tiff)) {
      continue;
    }

    // get width
    uint32_t width;
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width)) {
      // oh no
      continue;
    }

    // TODO: automatically identify label and thumbnail images via IMAGEDESCRIPTION
    // label image is tiled and hard-coded to dir=0, so filter it here
    if(dir == 0) {
      if(!add_associated_image(osr ? osr->associated_images : NULL, "label", tiff, err)) {
        g_prefix_error(err, "Can't read associated label image: ");
        goto FAIL;
      } else {
        continue; // don't add it to tiled (pyramidal) levels
      }
    }

    // thumbnail image is tiled and hard-coded to dir=1, so filter it here
    if(dir == 1) {
      if(!add_associated_image(osr ? osr->associated_images : NULL, "thumbnail", tiff, err)) {
        g_prefix_error(err, "Can't read associated thumbnail image: ");
        goto FAIL;
      } else {
        continue; // don't add it to tiled (pyramidal) levels
      }
    }

    // confirm it is either the first image, or reduced-resolution
    // unfortunately, SUBFILETYPE appears to be undefined
    // level name/value pair is in IMAGEDESCRIPTION
    char *imageDesc;
    char *level_value;
    if(!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &imageDesc)) {
      continue;
    }
    if(!find_property(imageDesc, "level", &level_value, 0)) {
      continue;
    }
    
    // verify that we can read this compression (hard fail if not)
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read compression scheme");
      goto FAIL;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }

    // use this opportunity to parse xmp data at level=0 (hard fail if it doesn't exist)
    char *xmpData;
    uint32_t *xmpData_size; 
    if(strcmp(level_value,"0") == 0) {

      bool status = TIFFGetField(tiff,TIFFTAG_XMLPACKET, &xmpData_size, &xmpData);

      // check if it containes iScan node before we invoke the parser
      if (!status || (strstr((const char *) xmpData, "<iScan") == NULL)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Ventana slide");
        goto FAIL;
      }

      if (!parse_xml_description(xmpData, osr, err)) {
        // unrecognizable xml
        goto FAIL;
      }
    }

    // push into list
    struct level *l = g_slice_new(struct level);
    l->directory = TIFFCurrentDirectory(tiff);
    l->width = width;
    level_list = g_list_prepend(level_list, l);
    level_count++;
  } while (TIFFReadDirectory(tiff));

  // sort tiled levels
  level_list = g_list_sort(level_list, width_compare);

  // copy levels in, while deleting the list
  levels = g_new(int32_t, level_count);
  for (int i = 0; i < level_count; i++) {
    struct level *l = level_list->data;
    level_list = g_list_delete_link(level_list, level_list);

    levels[i] = l->directory;
    g_slice_free(struct level, l);
  }

  g_assert(level_list == NULL);

  // all set, load up the TIFF-specific ops
  _openslide_add_tiff_ops(osr, tiff, levels[0],
			  0, NULL,
			  level_count, levels,
			  _openslide_generic_tiff_tilereader,
			  quickhash1);

  return true;

 FAIL:
  // free the level list
  for (GList *i = level_list; i != NULL; i = g_list_delete_link(i, i)) {
    g_slice_free(struct level, i->data);
  }

  g_free(levels);
  return false;
}
