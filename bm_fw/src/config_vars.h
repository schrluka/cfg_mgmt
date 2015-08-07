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
#ifndef __CONFIG_VARS__
#define __CONFIG_VARS__

#include <stdint.h>



/***********************************************************************************************************************
*   T Y P E S
*/

// structure for configuration variables
struct cfg_var
{
	int				id;			// identifier
	const char* 	name;		// human readable, unique name
	const char*		desc;		// human readable description (for help function etc)
	int32_t 		val;		// config value
	int32_t			min;		// min allowed value (hard coded)
	int32_t			max;		// max allowed value
};

typedef struct cfg_var cfgVar_t;



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
#define CFG_SW_TEST         3


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

#define CFG_DFLT_SW_TEST    {   .id=CFG_SW_TEST,  \
                                .name="sw_test", \
                                .desc="Test signals for Buck and Inj switches", \
                                .val=0, \
                                .min=0, \
                                .max=15 }

#endif
