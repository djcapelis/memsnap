/***********************************************************************
 * djc_defines.h                                                       *
 *                                                                     *
 * Standard macros I use  (Only one in this case)                      *
 *                                                                     *
 **********************************************************************/


#ifndef DJC_DEFINES_H
#define DJC_DEFINES_H

/* A simple macro that I can give a condition and have it jump to an err: handler if the condition is true */
#define err_chk(cond) if(cond) { goto err; }

#endif
