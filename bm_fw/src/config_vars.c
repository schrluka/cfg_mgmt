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
*   config_vars.c
*
*   Global array of configuration variables
*
******************************************************************************************************************************/


#include "config_vars.h"



/******************************************************************************************************************************
*   G L O B A L S
*/


// central array holding all config variables
// initialize all config variables here (this allocates memory and makes them usable)
cfgVar_t vars[] = { CFG_DFLT_VAR_1, \
                    CFG_DFLT_VAR_2, \
                    CFG_DFLT_SW_TEST};

// determine the number of variables specified above
const int n_vars = (sizeof(vars)/sizeof(cfgVar_t));



