/******************************************************************************************************************
*
*   CFG_MGMT  -  Configuration Variable Management for AMP using RPMSG
*
* (c) 2015 Lukas Schrittwieser (LS)
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
* cfg_mgmt.h
*
************************************************************************************************************************/

#ifndef __CFG_MGMT_MODULE__
#define __CFG_MGMT_MODULE__


// define a struct for data transfer between the kernel and the user space application
// a cfg_mgmt_var_t* is passed to various IOCTL calls
typedef struct __packed__
{
	uint32_t	val;	// variable value, min limit, max limit, etc depding on call
	int			ind;	// variable index, neg values mean unknown / no valid
	int			len;	// length of variable name in bytes, max given by rpmsg
	char*		data;	// pointer to string (for var name or description)
} cfg_mgmt_var_t;




#endif
