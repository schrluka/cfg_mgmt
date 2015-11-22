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
*   I N C L U D E S
*/
#include <stdint.h>
#include <stdbool.h>
//#include "config_vars.h"



/***********************************************************************************************************************
*   T Y P E S
*/

struct cfg_var;

// callback signature for registering callbacks on value read/write
// var: pointer to affected variable
// isread: var will be read by kernel once callback is finished
// data: pointer to private data which was passed to callback registration
void cfgCallback(struct cfg_var* var, bool isread, void* data);

typedef void(*cfgCallback_t)(struct cfg_var*, bool, void*);



// structure for configuration variables
struct cfg_var
{
	int				id;			// identifier
	const char* 	name;		// human readable, unique name
	const char*		desc;		// human readable description (for help function etc)
	int32_t 		val;		// config value
	int32_t			min;		// min allowed value (hard coded)
	int32_t			max;		// max allowed value
	cfgCallback_t   rd_cb;      // read access callback function
	void*           rd_cb_data; // read access cb private data
	cfgCallback_t   wr_cb;      // write access callback
	void*           wr_cb_data; // read access cb private data
};

typedef struct cfg_var cfgVar_t;



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
// if the new value exceeds the limits (min/max) of the variable it is limited accordingly
// trigCb: trigger a callback if this is true
// returns 1 on success and 0 on error
int cfgSetId(int id, int32_t val, bool trigCb);

// (un)register a callback function
// id: variable id for which this callback is registered
// cb: pointer to callback
// read: callback on read if true, write on false
// data: pointer will passed to callback when executed
int cfgSetCallback(int id, cfgCallback_t cb, bool read, void* data);

// generic callback handler which writes and read the variable value from the location pointed
// to by the data pointer passsed when registering it as CB for a variable
void cfgCpyCB(struct cfg_var* var, bool isread, void* data);

// generic callback for a float value, config variable
// gets divided by 1000, works for read and write
void cfgFloatMilliCB(struct cfg_var* var, bool isread, void* data);

#endif

