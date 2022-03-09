#ifndef ARTIFEX_EXTRACT_XML
#define ARTIFEX_EXTRACT_XML

/* Only for internal use by extract code.  */

#include "../include/extract_buffer.h"

#include "astring.h"


/* Things for representing XML. */

typedef struct {
    char*   name;
    char*   value;
} extract_xml_attribute_t;

/* Represents a single <...> XML tag plus trailing text. */
typedef struct {
    char*                       name;
    extract_xml_attribute_t*    attributes;
    int                         attributes_num;
    extract_astring_t           text;
} extract_xml_tag_t;


void extract_xml_tag_init(extract_xml_tag_t* tag);
/* Initialises tag. Will cause leak if tag contains data - in this case call
extract_xml_tag_free(). */

void extract_xml_tag_free(extract_alloc_t* alloc, extract_xml_tag_t* tag);
/* Frees tag and then calls extract_xml_tag_init(). */


int extract_xml_pparse_init(extract_alloc_t* alloc, extract_buffer_t* buffer, const char* first_line);
/* extract_xml_pparse_*(): simple XML 'pull' parser.

extract_xml_pparse_init() merely consumes the initial '<'. Thereafter
extract_xml_pparse_next() consumes the next '<' before returning the previous
tag. */

/* Opens specified file.

If first_line is not NULL, we check that it matches the first line in the file.

Returns -1 with errno=ESRCH if we fail to read the first '<' due to EOF.
*/


int extract_xml_pparse_next(extract_buffer_t* buffer, extract_xml_tag_t* out);
/* Returns the next XML tag.

Returns 0 with *out containing next tag; or -1 with errno set if error; or +1
with errno=ESRCH if EOF.

*out is initially passed to extract_xml_tag_free(), so *out must have been
initialised, e.g. by by extract_xml_tag_init(). */


char* extract_xml_tag_attributes_find(extract_xml_tag_t* tag, const char* name);
/* Returns pointer to value of specified attribute, or NULL if not found. */

int extract_xml_tag_attributes_find_float(
        extract_xml_tag_t*  tag,
        const char*         name,
        float*              o_out
        );
/* Finds float value of specified attribute, returning error if not found or
there is trailing text. */

int extract_xml_tag_attributes_find_double(
        extract_xml_tag_t*  tag,
        const char*         name,
        double*             o_out
        );
/* Finds double value of specified attribute, returning error if not found or there is
trailing text. */


/* Next few functions write to out-param and return zero on success, else
return -1 with errno set.

An error is returned if value is out of range or there is any trailing text. */

int extract_xml_str_to_llint(const char* text, long long* o_out);

int extract_xml_str_to_ullint(const char* text, unsigned long long* o_out);

int extract_xml_str_to_int(const char* text, int* o_out);

int extract_xml_str_to_uint(const char* text, unsigned* o_out);

int extract_xml_str_to_size(const char* text, size_t* o_out);

int extract_xml_str_to_double(const char* text, double* o_out);

int extract_xml_str_to_float(const char* text, float* o_out);


int extract_xml_tag_attributes_find_int(
        extract_xml_tag_t*  tag,
        const char*         name,
        int*                o_out
        );
/* Finds int value of specified attribute, returning error if not found. */

int extract_xml_tag_attributes_find_uint(
        extract_xml_tag_t*  tag,
        const char*         name,
        unsigned*           o_out
        );
/* Finds unsigned int value of specified attribute, returning error if not
found. */

int extract_xml_tag_attributes_find_size(
        extract_xml_tag_t*  tag,
        const char*         name,
        size_t*             o_out
        );
/* Finds unsigned int value of specified attribute, returning error if not
found. */

#endif
