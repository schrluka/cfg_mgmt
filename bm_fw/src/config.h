/***********************************************************************************************************************
*
* SCARABAEUS - Pick'n'Place Bot
*
* (c) 2013 Lukas Schrittwieser (LS)
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 2 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software
*    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*    Or see <http://www.gnu.org/licenses/>
*
************************************************************************************************************************
*
* config.h
*
* Header for config.c
*
************************************************************************************************************************
*
* 2013/6/27 - LS
*   started file
*
***********************************************************************************************************************/

#ifndef __CONFIG_H__
#define __CONFIG_H__

/***********************************************************************************************************************
*   I N C L U D ES
*/
#include <stdint.h>
#include "config_vars.h"


/***********************************************************************************************************************
*   D E F I N E S
*/

// maximum number of config variabless
//#define CFG_N_MAX	511




/***********************************************************************************************************************
*   P R O T O T Y P E S
*/

// Initialize config variable management and create rpmsg channel for communication with kernel
void cfgInit();

// read configuration value from variable with given id
// id: id of the config variable to be read
// val: pointer where variable will be stored (unchanged in case of error)
// return: 1 on success, 0 on error
int cfgGetValId(int id, int32_t* val);

// read configuration value from variable with given name
// name: pointer ot the variable name
// val: pointer where variable will be stored (unchanged in case of error)
// return: 1 on success, 0 on error
int cfgGetValName(const char* name, int32_t* val);

// Used to get a list of all variable names
// i: index counter, 0 returns the first name, 1 the second and so on. This is not the id value assigned to
//    the config variables.
// returns: pointer to string or NULL if no more names exist
const char* cfgGetNameList(int i);

// get variable struct with given id
// id: id of the variable to be looked up
// v: pointer to struct where information will be copied
// returns: 1 on success, 0 if id is not found or v is NULL
int cfgGetStructId(int id, cfgVar_t* v);

// get variable struct with given name
// id: id of the variable to be looked up
// v: pointer to struct where information will be copied
// returns: 1 on success, 0 if id is not found or v is NULL
int cfgGetStructName(const char* n, cfgVar_t* v);

// set variable (given by id) to new value
// if the new value exceeds the limits (min/max) of the variable it is trimmed accordingly
// Note that modification is only made to memory, not the eeprom
// returns 1 on success and 0 on error
int cfgSetId(int id, int32_t val);

#endif

