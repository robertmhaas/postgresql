/*-------------------------------------------------------------------------
 *
 * toasting.h
 *	  This file provides some definitions to support creation of toast tables
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/toasting.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TOASTING_H
#define TOASTING_H

#include "storage/lockdefs.h"

typedef struct Provenances Provenances;

/*
 * toasting.c prototypes
 */
extern void NewRelationCreateToastTable(Oid relOid, Datum reloptions,
										Provenances *provenances);
extern void NewHeapCreateToastTable(Oid relOid, Datum reloptions,
									LOCKMODE lockmode, Oid OIDOldToast,
									Provenances *provenances);
extern void AlterTableCreateToastTable(Oid relOid, Datum reloptions,
									   LOCKMODE lockmode,
									   Provenances *provenances);
extern void BootstrapToastTable(char *relName,
								Oid toastOid, Oid toastIndexOid);

#endif							/* TOASTING_H */
