/******************************************************************************************************************************
*
*   AMP Configuration Variable Management
*
*   Copyright (c) 2015 Lukas Schrittwieser
*
*   Permission is hereby granted, free of charge, to any person obtaining a copy
*   of this software and associated documentation files (the "Software"), to deal
*   in the Software without restriction, including without limitation the rights
*   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*   copies of the Software, and to permit persons to whom the Software is
*   furnished to do so, subject to the following conditions:
*
*   The above copyright notice and this permission notice shall be included in
*   all copies or substantial portions of the Software.
*
*   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
*   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
*   THE SOFTWARE.
*
*******************************************************************************************************************************
*
*   config_vars.h
*
*   Define configuration variables, i.e. ID codes, names, default values, limits, description, etc.
*
******************************************************************************************************************************/
#ifndef __CONFIG_VARS_H__
#define __CONFIG_VARS_H__

#include <stdint.h>
#include "config.h"



/******************************************************************************************************************************
*   G L O B A L S
*/

extern cfgVar_t vars[];

extern const int n_vars;



/******************************************************************************************************************************
*   I D   C O D E S
*/

// define unique IDs for all config variables, these are used only in the bare metal application

#define CFG_VAR_1           1
#define CFG_VAR_2           2
#define CFG_VAR_3           3
#define CFG_VAR_4           4
#define CFG_VAR_5           5
#define CFG_VAR_6           6
#define CFG_VAR_7           7



/******************************************************************************************************************************
*   V A R I B L E   S P E C S
*/

// define configuration variable defaults (these are used to init the global config variable structure)
// These hard coded defaults are used if no valid configuration memory (eeprom) is present
// create such a structure for each variable, then use it in config.c to initialize the global struct

#define CFG_DFLT_VAR_1	    {   .id=CFG_VAR_1,  \
                                .name="var_1", \
                                .desc="First config variable, possible values are 0 and 1", \
                                .val=0, \
                                .min=0, \
                                .max=1 }


#define CFG_DFLT_VAR_2	    {   .id=CFG_VAR_2,  \
                                .name="var_2", \
                                .desc="Second config variable, >0", \
                                .val=0, \
                                .min=0, \
                                .max=2147483647 }

#define CFG_DFLT_VAR_3	    {   .id=CFG_VAR_3,  \
                                .name="var_3", \
                                .desc="3rd config variable, >0", \
                                .val=0, \
                                .min=0, \
                                .max=2147483647 }

#define CFG_DFLT_VAR_4	    {   .id=CFG_VAR_4,  \
                                .name="var_4", \
                                .desc="4th config variable, >0", \
                                .val=0, \
                                .min=0, \
                                .max=2147483647 }

#define CFG_DFLT_VAR_5	    {   .id=CFG_VAR_5,  \
                                .name="var_5", \
                                .desc="5th config variable, >0", \
                                .val=0, \
                                .min=0, \
                                .max=2147483647 }

#define CFG_DFLT_VAR_6	    {   .id=CFG_VAR_6,  \
                                .name="var_6", \
                                .desc="6th config variable, >0", \
                                .val=0, \
                                .min=0, \
                                .max=2147483647 }

#define CFG_DFLT_VAR_7	    {   .id=CFG_VAR_7,  \
                                .name="var_7", \
                                .desc="7th config variable, >0", \
                                .val=0, \
                                .min=0, \
                                .max=2147483647 }


#endif
