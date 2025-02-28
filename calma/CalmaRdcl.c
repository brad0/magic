/*
 * CalmaReadcell.c --
 *
 * Input of Calma GDS-II stream format.
 * Processing for cells.
 *
 *     *********************************************************************
 *     * Copyright (C) 1985, 1990 Regents of the University of California. *
 *     * Permission to use, copy, modify, and distribute this              *
 *     * software and its documentation for any purpose and without        *
 *     * fee is hereby granted, provided that the above copyright          *
 *     * notice appear in all copies.  The University of California        *
 *     * makes no representations about the suitability of this            *
 *     * software for any purpose.  It is provided "as is" without         *
 *     * express or implied warranty.  Export of this software outside     *
 *     * of the United States of America may require an export license.    *
 *     *********************************************************************
 */

#ifndef lint
static char rcsid[] __attribute__ ((unused)) = "$Header: /usr/cvsroot/magic-8.0/calma/CalmaRdcl.c,v 1.5 2010/06/25 13:59:24 tim Exp $";
#endif  /* not lint */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "utils/magic.h"
#include "utils/geometry.h"
#include "tiles/tile.h"
#include "utils/utils.h"
#include "utils/hash.h"
#include "database/database.h"
#include "database/databaseInt.h"
#include "utils/malloc.h"
#include "utils/tech.h"
#include "cif/cif.h"
#include "cif/CIFint.h"
#include "cif/CIFread.h"
#include "utils/signals.h"
#include "windows/windows.h"
#include "dbwind/dbwind.h"
#include "utils/styles.h"
#include "textio/textio.h"
#include "calma/calmaInt.h"
#include "calma/calma.h"

/* C99 compat */
#include "drc/drc.h"

int calmaNonManhattan;
int CalmaFlattenLimit = 10;
int NameConvertErrors = 0;
bool CalmaRewound = FALSE;
TileTypeBitMask *CalmaMaskHints = NULL;

extern HashTable calmaDefInitHash;
extern int CalmaPolygonCount;

/* forward declarations */
int  calmaElementSref();
bool calmaParseElement();
void calmaUniqueCell();

/* Structure used when flattening the GDS hierarchy on read-in */

typedef struct {
    Plane *plane;
    Transform *trans;
} GDSCopyRec;

/* Added by NP 8/11/04 */

/*
 * ----------------------------------------------------------------------------
 *
 * calmaSetPosition --
 *
 *  This routine sets the file pointer calmaInputFile to the start
 *  of the CellDefinition "sname". It starts the search from the
 *  current position and looks forward to find the Cell Definition
 *  named "sname".
 *
 * Results:
 *	Current position of file pointer before it jumps to the
 *	definition of cell "sname" (if it exists, otherwise, returns
 *	end-of-file).
 *
 * Side Effects:
 *	The file position is set to the definition of cell "sname". If
 *	"sname" does not exist in the GDS stream file, the pointer is
 *	set to the end of the file.
 *
 * ----------------------------------------------------------------------------
 */

OFFTYPE
calmaSetPosition(sname)
    char *sname;
{
    OFFTYPE originalPos = 0, currentPos = 0;
    int nbytes, rtype;
    char *strname = NULL;
    int strRecSize = 0;
    bool found = FALSE;

    originalPos = FTELL(calmaInputFile);

    while (FEOF(calmaInputFile) == 0)
    {
        do
        {
             READRH(nbytes, rtype);      /* Read header	*/
             if (nbytes <= 0) break;

             /* Skip no of bytes in record header until
              * we reach to next structure record.
              */
             FSEEK(calmaInputFile, nbytes - CALMAHEADERLENGTH, SEEK_CUR);
        } while (rtype != CALMA_BGNSTR);
        if (nbytes <= 0) break;

        calmaReadStringRecord(CALMA_STRNAME, &strname);
        if ((strcmp(sname, strname)) == 0)
        {
             /* If name if structure matches with given name,
              * we got that Cell Defination. Set position of
              * file to start of that structure.
              */
             strRecSize = strlen(strname);
             if (strRecSize & 01) strRecSize++;
             FSEEK(calmaInputFile, -(nbytes + strRecSize + CALMAHEADERLENGTH),
				SEEK_CUR);
	     freeMagic(strname);
	     return originalPos;
        }
	freeMagic(strname);
    }

    /* Ran out of file.  It's possible that we were seeking ahead to a
     * definition that called something that was defined between it and
     * our previous position, so we will rewind the file and try again.
     * If that doesn't work, then the cell is not defined in the file.
     */

    if (originalPos != 0)
    {
	REWIND(calmaInputFile);
	CalmaRewound = TRUE;
	calmaSetPosition(sname);
	if (!CalmaPostOrder)
	    CalmaReadError("Rewinding input.  Cells may have been instanced before "
		    "they were defined.  Consider using \"gds ordering on\".\n");
	return originalPos;
    }

    /* Avoid generating an error message in the case that the cell is not in
     * the GDS file but is in memory.  Assume that such a case is intentional,
     * meaning the GDS library is an addendum and the proper library it
     * depends on was read first as intended.
     */
    if (DBCellLookDef(sname) == NULL)
        CalmaReadError("Cell \"%s\" is used but not defined in this file.\n",
		sname);

    return originalPos;
}

/* Added by NP 8/11/04 */

/*
 * ----------------------------------------------------------------------------
 *
 * calmaNextCell --
 *
 *   This routine sets the file pointer to next Cell Definition
 *   in the GDS stream. It goes only in the forward direction
 *   from the current position of the file pointer.
 *
 * Results:
 *	None.
 *
 * Side Effects:
 *	File pointer set to start of next Cell definition.
 *
 * ----------------------------------------------------------------------------
 */

void
calmaNextCell()
{
    int nbytes, rtype;

    if (FEOF(calmaInputFile) == 0)
    {
        do
        {
             READRH(nbytes, rtype);      /* Read header */
             if (nbytes <= 0)
             {
                /* We have reached the end of the file unexpectedly.
		 * try to set the file pointer somewhere sane, but
		 * it will likely dump an error later on.
		 */
                FSEEK(calmaInputFile, -(CALMAHEADERLENGTH), SEEK_END);
                return;
             }

             /* Skip no. of bytes in record header to reach the next
	      * structure record.
              */
             FSEEK(calmaInputFile, nbytes - CALMAHEADERLENGTH, SEEK_CUR);
        } while((rtype != CALMA_BGNSTR) && (rtype != CALMA_ENDLIB));

	FSEEK(calmaInputFile, -nbytes, SEEK_CUR);
    }
}

/*
 * ----------------------------------------------------------------------------
 *
 * calmaExact --
 *
 * When dictated to flatten small cells (e.g., vias which have been placed
 * into their own cells to make use of GDS arrays), instead of calling
 * CIFPaintCurrent(), we just transfer the planes of cifCurReadPlanes to
 * the current cell, and swap the planes from the current cell into the
 * pointer array for cifCurReadPlanes, so we don't have to free and
 * reallocate any memory for the operation.  Flag the cell as being a
 * CIF cell, so that it can be destroyed after read-in and flattening.
 *
 * Results:
 *	Pointer to the plane structure created to hold the GDS data.
 *
 * Side Effects:
 *	Swaps the planes of cifReadCellDef with cifCurReadPlanes.
 *
 * ----------------------------------------------------------------------------
 */

Plane **
calmaExact()
{
    int pNum;
    Plane *newplane;
    Plane **parray;
    int gdsCopyPaintFunc();	/* Forward reference */

    parray = (Plane **)mallocMagic(MAXCIFRLAYERS * sizeof(Plane *));

    for (pNum = 0; pNum < MAXCIFRLAYERS; pNum++)
    {
	if (cifCurReadPlanes[pNum] != NULL)
	{
	    GDSCopyRec gdsCopyRec;

	    newplane = DBNewPlane((ClientData) TT_SPACE);
	    DBClearPaintPlane(newplane);

	    gdsCopyRec.plane = newplane;
	    gdsCopyRec.trans = NULL;
	    DBSrPaintArea((Tile *)NULL, cifCurReadPlanes[pNum], &TiPlaneRect,
		&DBAllButSpaceBits, gdsCopyPaintFunc, &gdsCopyRec);
	    parray[pNum] = newplane;
	}
	else
	    parray[pNum] = NULL;
    }

    /* Clear out the current paint planes for the next cell */
    for (pNum = 0; pNum < MAXCIFRLAYERS; pNum++)
	DBClearPaintPlane(cifCurReadPlanes[pNum]);

    return parray;
}

/*
 * ----------------------------------------------------------------------------
 *
 * calmaFlattenPolygonFunc --
 *
 * Polygons have been dropped into subcells by default for
 * efficiency in reading.  If the "subcell polygons" option
 * has not been selected, then flatten these cells into the
 * layout and delete the cells.  This seems inefficient but
 * in fact can be much faster than reading the polygons
 * directly into the cell from GDS.
 *
 * Return value:
 *	Return 0 to keep the search going
 *
 * Side effects:
 *	Polygons are copied from use->cu_def to parent.
 *	use->cu_def is deleted.
 *
 * ----------------------------------------------------------------------------
 */

int
calmaFlattenPolygonFunc(use, parent)
    CellUse *use;
    CellDef *parent;
{
    int i;
    CellUse dummy;
    SearchContext scx;
    HashEntry *he;

    if (use->cu_def == NULL || use->cu_def->cd_name == NULL) return 0;
    if (strncmp(use->cu_def->cd_name, "polygon", 7)) return 0;

    dummy.cu_transform = GeoIdentityTransform;
    dummy.cu_id = NULL;
    dummy.cu_def = parent;
    scx.scx_use = use;
    scx.scx_area = use->cu_bbox;
    scx.scx_trans = GeoIdentityTransform;
    DBCellCopyAllPaint(&scx, &DBAllButSpaceAndDRCBits, 0, &dummy);
    DBDeleteCellNoModify(use);
    HashRemove(&CifCellTable, use->cu_def->cd_name);
    /* There should only be one use, so it can just be cleared */
    use->cu_def->cd_parents = (CellUse *)NULL;
    DBCellDeleteDef(use->cu_def);

    return 0;	/* Keep the search going */
}

/*
 * ----------------------------------------------------------------------------
 *
 * calmaParseStructure --
 *
 * Process a complete GDS-II structure (cell) including its closing
 * CALMA_ENDSTR record.  In the event of a syntax error, we skip
 * ahead to the closing CALMA_ENDSTR, output a warning, and keep going.
 *
 * Results:
 *	TRUE if successful, FALSE if the next item in the input is
 *	not a structure.
 *
 * Side effects:
 *	Reads a new cell.
 *	Consumes input.
 *
 * ----------------------------------------------------------------------------
 */

bool
calmaParseStructure(filename)
    char *filename;		/* Name of the GDS file read */
{
    static int structs[] = { CALMA_STRCLASS, CALMA_STRTYPE, -1 };
    int nbytes, rtype, nsrefs, osrefs, npaths;
    char *strname = NULL;
    HashEntry *he;
    int timestampval = 0;
    int suffix;
    int mfactor;
    int locPolygonCount;
    OFFTYPE filepos;
    bool was_called = FALSE;
    bool was_initialized;
    bool predefined;
    bool do_flatten;
    CellDef *def;
    Label *lab;

    locPolygonCount = CalmaPolygonCount;

    /* Make sure this is a structure; if not, let the caller know we're done */
    PEEKRH(nbytes, rtype);
    if (nbytes <= 0 || rtype != CALMA_BGNSTR)
	return (FALSE);

    /* Read the structure name */
    was_initialized = FALSE;
    predefined = FALSE;
    if (!calmaReadStampRecord(CALMA_BGNSTR, &timestampval)) goto syntaxerror;
    if (!calmaReadStringRecord(CALMA_STRNAME, &strname)) goto syntaxerror;
    TxPrintf("Reading \"%s\".\n", strname);

    /* Used for read-only and annotated LEF views */
    filepos = FTELL(calmaInputFile);

    /* Set up the cell definition */
    he = HashFind(&calmaDefInitHash, strname);
    if ((def = (CellDef *)HashGetValue(he)) != NULL)
    {
	if (def->cd_flags & CDPROCESSEDGDS)
	{
	    /* If cell definition was marked as processed, then skip	*/
	    /* (NOTE:  This is probably a bad policy in general)	*/
	    /* However, if post-ordering is set, then this cell was	*/
	    /* probably just read earlier, so don't gripe about it.	*/

	    if (!CalmaPostOrder && !CalmaRewound)
	    {
		CalmaReadError("Cell \"%s\" was already defined in this file.\n",
				strname);
		CalmaReadError("Ignoring duplicate definition\n");
	    }
	    calmaNextCell();
	    return TRUE;
	}
	else
	{
	    char *newname;

	    CalmaReadError("Cell \"%s\" was already defined in this file.\n",
				strname);
	    newname = (char *)mallocMagic(strlen(strname) + 20);
	    for (suffix = 1; HashGetValue(he) != NULL; suffix++)
	    {
		(void) sprintf(newname, "%s_%d", strname, suffix);
		he = HashFind(&calmaDefInitHash, newname);
	    }
	    CalmaReadError("Giving this cell a new name: %s\n", newname);
	    freeMagic(strname);
	    strname = mallocMagic(strlen(newname) + 1);
	    strcpy(strname, newname);
	    freeMagic(newname);
	}
    }
    if (CalmaUnique) calmaUniqueCell(strname);	/* Ensure uniqueness */
    cifReadCellDef = calmaFindCell(strname, &was_called, &predefined);
    HashSetValue(he, cifReadCellDef);

    if (predefined == TRUE)
    {
	bool isAbstract;

	/* If the cell was predefined, the "noduplicates" option was
	 * invoked, and the existing cell is an abstract view, then
	 * annotate the cell with the GDS file pointers to the cell
	 * data, and the GDS filename.
	 */
	DBPropGet(cifReadCellDef, "LEFview", &isAbstract);
	if (!isAbstract)
	{
	    calmaNextCell();
	    return TRUE;
	}
	calmaSkipTo(CALMA_ENDSTR);
    }
    else
    {
	DBCellClearDef(cifReadCellDef);
	DBCellSetAvail(cifReadCellDef);
	cifCurReadPlanes = cifSubcellPlanes;
	cifReadCellDef->cd_flags &= ~CDDEREFERENCE;

	/* Set timestamp from the GDS cell's creation time	*/
	/* timestamp, unless "calma datestamp" was specified	*/
	/* with a value, in which case use that value.		*/

	if (CalmaDateStamp == NULL)
	    cifReadCellDef->cd_timestamp = timestampval;
	else
	{
	    cifReadCellDef->cd_timestamp = *CalmaDateStamp;
	    if (*CalmaDateStamp != (time_t)0)
		cifReadCellDef->cd_flags |= CDFIXEDSTAMP;
	}

	/* For read-only cells, set flag in def */
	if (CalmaReadOnly)
	    cifReadCellDef->cd_flags |= CDVENDORGDS;

	/* Skip CALMA_STRCLASS or CALMA_STRTYPE */
	calmaSkipSet(structs);

	/* Initialize the hash table for layer errors */
	HashInit(&calmaLayerHash, 32, sizeof (CalmaLayerType) / sizeof (unsigned));
	was_initialized = TRUE;

	/* Body of structure: a sequence of elements */
	osrefs = nsrefs = 0;
	npaths = 0;
	calmaNonManhattan = 0;

	while (calmaParseElement(filename, &nsrefs, &npaths))
    	{
	    if (SigInterruptPending)
	    	goto done;
	    if (nsrefs > osrefs && (nsrefs % 5000) == 0)
	   	TxPrintf("    %d uses\n", nsrefs);
	    osrefs = nsrefs;
	    calmaNonManhattan = 0;
	}
    }

    if (CalmaReadOnly || predefined)
    {
	char cstring[1024];

	/* Writing the file position into a string is slow, but */
	/* it prevents requiring special handling when printing	*/
	/* out the properties.					*/

	char *fpcopy = (char *)mallocMagic(20);
	char *fncopy;

	/* Substitute variable for PDK path or ~ for home directory	*/
	/* the same way that cell references are handled in .mag files.	*/
	DBPathSubstitute(filename, cstring, cifReadCellDef);
	fncopy = StrDup(NULL, cstring);
	sprintf(fpcopy, "%"DLONG_PREFIX"d", (dlong) filepos);
	DBPropPut(cifReadCellDef, "GDS_START", (ClientData)fpcopy);

	fpcopy = (char *)mallocMagic(20);
	filepos = FTELL(calmaInputFile);
	sprintf(fpcopy, "%"DLONG_PREFIX"d", (dlong) filepos);
	DBPropPut(cifReadCellDef, "GDS_END", (ClientData)fpcopy);

	DBPropPut(cifReadCellDef, "GDS_FILE", (ClientData)fncopy);

    	if (predefined)
    	{
    	    if (strname != NULL) freeMagic(strname);
	    return TRUE;
        }
    }

    /* Check if the cell name matches the pattern list of cells to flatten */

    do_flatten = FALSE;
    if ((CalmaFlattenUsesByName != NULL) && (!was_called))
    {
	int i = 0;
	char *pattern;

	while (TRUE)
	{
	    pattern = CalmaFlattenUsesByName[i];
	    if (pattern == NULL) break;
	    i++;

	    /* Check pattern against strname */
	    if (Match(pattern, strname))
	    {
		do_flatten = TRUE;
		break;
	    }
	}
    }
    if (CalmaFlattenUses && (!was_called) && (npaths < CalmaFlattenLimit)
                && (nsrefs == 0))
	do_flatten = TRUE;

    /* Done with strname */
    if (strname != NULL) freeMagic(strname);

    /* Make sure the structure ends with an ENDSTR record */
    if (!calmaSkipExact(CALMA_ENDSTR)) goto syntaxerror;

    /*
     * Don't paint now, just keep the CIF planes, and flatten the
     * cell by painting when instanced.  But---if this cell was
     * instanced before it was defined, then it can't be flattened.
     */

    if (do_flatten)
    {
	/* If CDFLATGDS is already set, may need to remove	*/
	/* existing planes and free memory.			*/

	if ((cifReadCellDef->cd_client != (ClientData)0) &&
		(cifReadCellDef->cd_flags & CDFLATGDS))
	{
	    Plane **cifplanes = (Plane **)cifReadCellDef->cd_client;
	    int pNum;

            for (pNum = 0; pNum < MAXCIFRLAYERS; pNum++)
            {
		if (cifplanes[pNum] != NULL)
                {
                    DBFreePaintPlane(cifplanes[pNum]);
                    TiFreePlane(cifplanes[pNum]);
                }
            }
            freeMagic((char *)cifReadCellDef->cd_client);
            cifReadCellDef->cd_client = (ClientData)0;
	}

	TxPrintf("Saving contents of cell %s\n", cifReadCellDef->cd_name);
	cifReadCellDef->cd_client = (ClientData) calmaExact();
	cifReadCellDef->cd_flags |= CDFLATGDS;
	cifReadCellDef->cd_flags &= ~CDFLATTENED;

	/* Remove any labels in this cell */
	DBEraseLabel(cifReadCellDef, &TiPlaneRect, &DBAllTypeBits, NULL);
    }
    else
    {
	/*
	 * Do the geometrical processing and paint this material back into
	 * the appropriate cell of the database.
	 */

	CIFPaintCurrent(FILE_CALMA);
    }

    /* Check for empty string labels that are caused by pin geometry that
     * did not get any corresponding text type, and remove them.
     */
    DBEraseLabelsByContent(cifReadCellDef, NULL, -1, "");

    if ((CalmaSubcellPolygons == CALMA_POLYGON_TEMP) &&
			(locPolygonCount < CalmaPolygonCount))
	DBCellEnum(cifReadCellDef, calmaFlattenPolygonFunc, (ClientData)cifReadCellDef);

    DBAdjustLabelsNew(cifReadCellDef, &TiPlaneRect,
	      (cifCurReadStyle->crs_flags & CRF_NO_RECONNECT_LABELS) ? 1 : 0);
    DBReComputeBbox(cifReadCellDef);

    /* Don't bother to register with DRC if we're going to delete the	*/
    /* cell, or if the cell is read-only, or if "calma drcnocheck true"	*/
    /* has been issued.							*/

    if (!CalmaReadOnly && !CalmaNoDRCCheck)
	DRCCheckThis(cifReadCellDef, TT_CHECKPAINT, &cifReadCellDef->cd_bbox);

    DBWAreaChanged(cifReadCellDef, &cifReadCellDef->cd_bbox,
	DBW_ALLWINDOWS, &DBAllButSpaceBits);

    /* Note:  This sets the CDGETNEWSTAMP flag, but if the timestamp is
     * not zero, then it gets cleared in CIFReadCellCleanup().
     */
    DBCellSetModified(cifReadCellDef, TRUE);

    /*
     * Assign use-identifiers to all the cell uses.
     * These identifiers are generated so as to be
     * unique.
     */
    DBGenerateUniqueIds(cifReadCellDef, FALSE);
    cifReadCellDef->cd_flags |= CDPROCESSEDGDS;

done:
    HashKill(&calmaLayerHash);
    return (TRUE);

    /* Syntax error: skip to CALMA_ENDSTR */
syntaxerror:
    if (was_initialized == TRUE) HashKill(&calmaLayerHash);
    return (calmaSkipTo(CALMA_ENDSTR));
}


/*
 * ----------------------------------------------------------------------------
 *
 * calmaParseElement --
 *
 * Process one element from a GDS-II structure, including its
 * trailing CALMA_ENDEL record.  In the event of a syntax error, we skip
 * ahead to the closing CALMA_ENDEL, output a warning, and keep going.
 *
 * Results:
 *	TRUE if we processed an element, FALSE when we reach something that
 *	is not an element.  In the latter case, we leave the non-element
 *	record unconsumed.
 *
 * Side effects:
 *	Consumes input.
 *	Depends on the kind of element encountered.
 *	If we process a SREF or AREF, increment *pnsrefs.
 *
 * ----------------------------------------------------------------------------
 */

bool
calmaParseElement(filename, pnsrefs, pnpaths)
    char *filename;
    int *pnsrefs, *pnpaths;
{
    static int node[] = { CALMA_ELFLAGS, CALMA_PLEX, CALMA_LAYER,
			  CALMA_NODETYPE, CALMA_XY, -1 };
    int nbytes, rtype, madeinst;

    READRH(nbytes, rtype);
    if (nbytes < 0)
    {
	CalmaReadError("Unexpected EOF.\n");
	return (FALSE);
    }

    switch (rtype)
    {
	case CALMA_AREF:
	case CALMA_SREF:
	    madeinst = calmaElementSref(filename);
	    if (madeinst >= 0)
		(*pnsrefs) += madeinst;
	    break;
	case CALMA_BOUNDARY:
	    calmaElementBoundary();
	    (*pnpaths)++;
	    break;
	case CALMA_BOX:
	    calmaElementBox();
	    (*pnpaths)++;
	    break;
	case CALMA_PATH:
	    calmaElementPath();
	    (*pnpaths)++;
	    break;
	case CALMA_TEXT:
	    calmaElementText();
	    break;
	case CALMA_NODE:
	    CalmaReadError("NODE elements not supported: skipping.\n");
	    calmaSkipSet(node);
	    break;
	default:
	    UNREADRH(nbytes, rtype);
	    return (FALSE);
    }

    return (calmaSkipTo(CALMA_ENDEL));
}

/*
 * ----------------------------------------------------------------------------
 *
 * Callback procedure for enumerating any paint in a cell.  Used to find if
 * a cell needs to be retained after being flattened into the parent cell.
 *
 * Returns 1 always.  Only called if a non-space tile was encountered.
 *
 * ----------------------------------------------------------------------------
 */

int
calmaEnumFunc(tile, plane)
    Tile *tile;
    int *plane;
{
    return 1;
}

/*
 * ----------------------------------------------------------------------------
 *
 * calmaElementSref --
 *
 * Process a structure reference (either CALMA_SREF or CALMA_AREF).
 *
 * Results:
 *	Returns 1 if an actual child instance was generated, 0 if
 *	only the child geometry was copied up, and -1 if an error
 * 	occurred.
 *
 * Side effects:
 *	Consumes input.
 *	Adds a new cell use to the current def.
 *
 * ----------------------------------------------------------------------------
 */

int
calmaElementSref(filename)
    char *filename;
{
    int nbytes, rtype, cols, rows, nref, n, i, savescale;
    int xlo, ylo, xhi, yhi, xsep, ysep;
    bool madeinst = FALSE;
    char *sname = NULL;
    bool isArray = FALSE;
    Transform trans, tinv;
    Point refarray[3], refunscaled[3], p;
    CellUse *use;
    CellDef *def;
    int gdsCopyPaintFunc();	/* Forward reference */
    int gdsHasUses();		/* Forward reference */
    /* Added by NP */
    char *useid = NULL, *arraystr = NULL;
    int propAttrType;

    /* Skip CALMA_ELFLAGS, CALMA_PLEX */
    calmaSkipSet(calmaElementIgnore);

    /* Read subcell name */
    if (!calmaReadStringRecord(CALMA_SNAME, &sname))
	return -1;

    /*
     * Create a new cell use with this transform.  If the
     * cell being referenced doesn't exist, create it.
     * Don't give it a use-id; we will do that only after
     * we've placed all cells.
     */

    def = calmaLookCell(sname);
    if (!def && (CalmaPostOrder || CalmaFlattenUses || (CalmaFlattenUsesByName != NULL)))
    {
	/* Force the GDS parser to read the cell definition in
	 * post-order.  If cellname "sname" is not defined before
	 * it is used, then read the cell definition by jumping
	 * to its position in the stream file, reading that
	 * instance, and then returning to the current file
	 * position.  Note that this routine should be optimized
	 * for the likely case where many cells are defined at
	 * the end of the file; otherwise it will slow down the
	 * read process excessively for large GDS files by
	 * constantly moving from beginning to end of the file.
	 *
	 * Added by Nishit 8/16/2004
	 */

	OFFTYPE originalFilePos = calmaSetPosition(sname);
	if (!FEOF(calmaInputFile))
	{
	    HashTable OrigCalmaLayerHash;
	    int crsMultiplier = cifCurReadStyle->crs_multiplier;
	    char *currentSname = cifReadCellDef->cd_name;
	    Plane **savePlanes;

	    OrigCalmaLayerHash = calmaLayerHash;
	    savePlanes = calmaExact();

	    /* Read cell definition "sname". */
	    calmaParseStructure(filename);

	    /* Put things back to the way they were. */
	    FSEEK(calmaInputFile, originalFilePos, SEEK_SET);
	    cifReadCellDef = calmaLookCell(currentSname);
	    def = calmaLookCell(sname);
	    cifCurReadPlanes = savePlanes;
	    calmaLayerHash = OrigCalmaLayerHash;
	    if (crsMultiplier != cifCurReadStyle->crs_multiplier)
	    {
		int scalen, scaled;
		if (crsMultiplier > cifCurReadStyle->crs_multiplier)
		{
		    scalen = crsMultiplier / cifCurReadStyle->crs_multiplier;
		    scaled = 1;
		}
		else
		{
		    scalen = 1;
		    scaled = cifCurReadStyle->crs_multiplier / crsMultiplier;
		}
		CIFScalePlanes(scaled, scalen, cifCurReadPlanes);
	    }
	}
	else
	{
	    /* This is redundant messaging */
	    // TxPrintf("Cell definition %s does not exist!\n", sname);
	    FSEEK(calmaInputFile, originalFilePos, SEEK_SET);
	    def = calmaFindCell(sname, NULL, NULL);
	    /* Cell flags set to "dereferenced" in case there is no	*/
	    /* definition in the GDS file.  If there is a definition	*/
	    /* made after the instance, then the flag will be cleared.	*/
	    def->cd_flags |= CDDEREFERENCE;
	}
    }

    if (!def) def = calmaFindCell(sname, NULL, NULL);

    if (DBIsAncestor(def, cifReadCellDef))
    {
	CalmaReadError("Cell %s is an ancestor of %s",
			def->cd_name, cifReadCellDef->cd_name);
	CalmaReadError(" and can't be used as a subcell.\n");
	CalmaReadError("(Use skipped)\n");
	return -1;
    }

    /* Read subcell transform */
    if (!calmaReadTransform(&trans, sname))
    {
	printf("Couldn't read transform.\n");
	return -1;
    }

    /* Get number of columns and rows if array */
    cols = rows = 0;  /* For half-smart compilers that complain otherwise. */
    READRH(nbytes, rtype);
    if (nbytes < 0) return -1;
    if (rtype == CALMA_COLROW)
    {
	isArray = TRUE;
	READI2(cols);
	READI2(rows);
	xlo = 0; xhi = cols - 1;
	ylo = 0; yhi = rows - 1;
	if (FEOF(calmaInputFile)) return -1;
	(void) calmaSkipBytes(nbytes - CALMAHEADERLENGTH - 4);
    }
    else
    {
	UNREADRH(nbytes, rtype);
    }

    /*
     * Read reference points.
     * For subcells, there will be a single reference point.
     * For arrays, there will be three; for their meanings, see below.
     */
    READRH(nbytes, rtype)
    if (nbytes < 0) return -1;
    if (rtype != CALMA_XY)
    {
	calmaUnexpected(CALMA_XY, rtype);
	return -1;
    }

    /* Length of remainder of record */
    nbytes -= CALMAHEADERLENGTH;

    /*
     * Read the reference points for the SREF/AREF.
     * Scale down by cifCurReadStyle->crs_scaleFactor, but complain
     * if they don't scale exactly.
     * Make sure we only read three points.
     */
    nref = nbytes / 8;
    if (nref > 3)
    {
	CalmaReadError("Too many points (%d) in SREF/AREF\n", nref);
	nref = 3;
    }
    else if (nref < 1)
    {
	CalmaReadError("Missing reference points in SREF/AREF (using 0,0)\n");
	refarray[0].p_x = refarray[0].p_y = 0;
	refarray[1].p_x = refarray[1].p_y = 0;
	refarray[2].p_x = refarray[2].p_y = 0;
    }

    /* If this is a cell reference, then we scale to magic coordinates
     * and place the cell in the magic database.  However, if this is
     * a cell to be flattened a la "gds flatten", then we keep the GDS
     * coordinates, and don't scale to the magic database.
     */

    for (n = 0; n < nref; n++)
    {
	savescale = cifCurReadStyle->crs_scaleFactor;
	calmaReadPoint(&refarray[n], 1);
	refunscaled[n] = refarray[n];	// Save for CDFLATGDS cells
	refarray[n].p_x = CIFScaleCoord(refarray[n].p_x, COORD_EXACT);
	if (savescale != cifCurReadStyle->crs_scaleFactor)
	{
	    for (i = 0; i < n; i++)
	    {
		refarray[i].p_x *= (savescale / cifCurReadStyle->crs_scaleFactor);
		refarray[i].p_y *= (savescale / cifCurReadStyle->crs_scaleFactor);
	    }
	    savescale = cifCurReadStyle->crs_scaleFactor;
	}
	refarray[n].p_y = CIFScaleCoord(refarray[n].p_y, COORD_EXACT);
	if (savescale != cifCurReadStyle->crs_scaleFactor)
	{
	    for (i = 0; i < n; i++)
	    {
		refarray[i].p_x *= (savescale / cifCurReadStyle->crs_scaleFactor);
		refarray[i].p_y *= (savescale / cifCurReadStyle->crs_scaleFactor);
	    }
	    refarray[n].p_x *= (savescale / cifCurReadStyle->crs_scaleFactor);
	}

	if (FEOF(calmaInputFile))
	    return -1;
    }

    /* Skip remainder */
    nbytes -= nref * 8;
    if (nbytes)
	(void) calmaSkipBytes(nbytes);

    /* Added by NP --- parse PROPATTR and PROPVALUE record types */
    /* The PROPATTR types 98 and 99 are defined internally to	 */
    /* magic and are used to retain information that cannot be	 */
    /* saved to the GDS file in any other manner.		 */

    while (1)
    {
	READRH(nbytes, rtype);
	if (nbytes < 0) return -1;
	if (rtype == CALMA_PROPATTR)
	{
	    READI2(propAttrType);
	    if (propAttrType == CALMA_PROP_USENAME ||
		 propAttrType == CALMA_PROP_USENAME_STD)
	    {
		char *s;

		if (!calmaReadStringRecord(CALMA_PROPVALUE, &useid))
		    return -1;

		/* Magic prohibits comma and slash from use names */
		for (s = useid; *s; s++)
		    if (*s == '/' || *s == ',')
		    {
			if (NameConvertErrors < 100)
			    TxPrintf("\"%c\" character cannot be used in instance name; "
					"converting to underscore\n", *s);
			else if (NameConvertErrors == 100)
			    TxPrintf("More than 100 character changes; not reporting"
					" further errors.\n");
			*s = '_';
			NameConvertErrors++;
		    }
	    }
	    else if (propAttrType == CALMA_PROP_ARRAY_LIMITS)
	    {
		if (!calmaReadStringRecord(CALMA_PROPVALUE, &arraystr))
		    return -1;
		else if (arraystr)
		{
		    int xl, xh, yl, yh;
		    if (sscanf(arraystr, "%d_%d_%d_%d", &xl, &xh, &yl, &yh)
				!= 4)
			TxError("Malformed \"array\" property ignored: %s",
				arraystr);
		    else
		    {
			xlo = xl;
			ylo = yl;
			xhi = xh;
			yhi = yh;
		    }
		}
	    }
	}
	else
	{
	    UNREADRH(nbytes, rtype);
	    break;
	}
    }

    /* Transform manipulation.  Run through this twice if necessary.	*/
    /* The second time will use the unscaled transform to compute	*/
    /* the coordinates of saved geometry for a CD_FLATGDS cell.		*/

    for (i = 0; i < 2; i++)
    {
	if (i == 1)
	{
	    for (n = 0; n < nref; n++)
		refarray[n] = refunscaled[n]; 	// Restore for CDFLATGDS cells
	}

	/*
	 * Figure out the inter-element spacing of array elements,
	 * and also the translation part of the transform.
	 * The first reference point for both SREFs and AREFs is the
	 * translation of the use's or array's lower-left.
	 */

	trans.t_c = refarray[0].p_x;
	trans.t_f = refarray[0].p_y;
	GeoInvertTrans(&trans, &tinv);
	if (isArray)
	{
	    /*
	     * The remaining two points for an array are displaced from
	     * the first reference point by:
	     *    - the inter-column spacing times the number of columns,
	     *    - the inter-row spacing times the number of rows.
	     */
	    xsep = ysep = 0;
	    if (cols)
	    {
		GeoTransPoint(&tinv, &refarray[1], &p);
		if (p.p_x % cols)
		{
		    n = (p.p_x + (cols+1)/2) / cols;
		    CalmaReadError("# cols doesn't divide displacement ref pt\n");
		    CalmaReadError("    %d / %d -> %d\n", p.p_x, cols, n);
		    xsep = n;
		}
		else xsep = p.p_x / cols;
	    }
	    if (rows)
	    {
		GeoTransPoint(&tinv, &refarray[2], &p);
		if (p.p_y % rows)
		{
		    n = (p.p_y + (rows+1)/2) / rows;
		    CalmaReadError("# rows doesn't divide displacement ref pt\n");
		    CalmaReadError("    %d / %d -> %d\n", p.p_y, rows, n);
		    ysep = n;
		}
		ysep = p.p_y / rows;
	    }
	}

	/*
	 * Check for cells which have *not* been marked for flattening
	 */

	if (!(def->cd_flags & CDFLATGDS))
	{
	    use = DBCellNewUse(def, (useid) ? useid : (char *) NULL);
	    if (isArray)
		DBMakeArray(use, &GeoIdentityTransform, xlo, ylo, xhi, yhi, xsep, ysep);
	    DBSetTrans(use, &trans);
	    if (DBCellFindDup(use, cifReadCellDef) != NULL)
	    {
		DBCellDeleteUse(use);
		CalmaReadError("Warning: cell \"%s\" placed on top of"
				" itself.  Ignoring the extra one.\n", def->cd_name);
	    }
	    else
		DBPlaceCell(use, cifReadCellDef);

	    break;	// No need to do 2nd loop
	}

	else if (i == 1)
	{
	    Plane **gdsplanes = (Plane **)def->cd_client;
	    GDSCopyRec gdsCopyRec;
	    int pNum;
	    bool hasUses;

	    // Mark cell as flattened (at least once)
	    def->cd_flags |= CDFLATTENED;

	    for (pNum = 0; pNum < MAXCIFRLAYERS; pNum++)
	    {
		if ((def->cd_client != (ClientData)0) && (gdsplanes[pNum] != NULL))
		{
		    gdsCopyRec.plane = cifCurReadPlanes[pNum];
		    if (isArray)
		    {
			Transform artrans;
			int x, y;

			gdsCopyRec.trans = &artrans;

			for (x = 0; x < cols; x++)
			    for (y = 0; y < rows; y++)
			    {
				GeoTransTranslate((x * xsep), (y * ysep), &trans,
					&artrans);
				DBSrPaintArea((Tile *)NULL, gdsplanes[pNum],
					&TiPlaneRect, &DBAllButSpaceBits,
					gdsCopyPaintFunc, &gdsCopyRec);
			    }
		    }
		    else
		    {
			gdsCopyRec.trans = &trans;
			DBSrPaintArea((Tile *)NULL, gdsplanes[pNum], &TiPlaneRect,
				&DBAllButSpaceBits, gdsCopyPaintFunc, &gdsCopyRec);
		    }
		}
	    }
	}

	/* When not reading with VENDORGDS, if a cell has contents	*/
	/* other than the paint to be flattened, then also generate an	*/
	/* instance of the cell.  Otherwise (with VENDORGDS), always 	*/
	/* generate cell instances.  Note that only paint and cells	*/
	/* are counted, not labels (see below).				*/

	else if (!(def->cd_flags & CDVENDORGDS))
	{
	    int plane;
	    for (plane = PL_TECHDEPBASE; plane < DBNumPlanes; plane++)
		if (DBSrPaintArea((Tile *)NULL, def->cd_planes[plane], &TiPlaneRect,
			&DBAllButSpaceAndDRCBits, calmaEnumFunc, (ClientData)NULL))
		    break;

	    if ((plane < DBNumPlanes) || DBCellEnum(def, gdsHasUses, (ClientData)NULL))
	    {
		use = DBCellNewUse(def, (useid) ? useid : (char *) NULL);
		if (isArray)
		    DBMakeArray(use, &GeoIdentityTransform, xlo, ylo, xhi, yhi, xsep, ysep);
		DBSetTrans(use, &trans);
		if (DBCellFindDup(use, cifReadCellDef) != NULL)
		{
		    DBCellDeleteUse(use);
		    CalmaReadError("Warning: cell \"%s\" placed on top of"
				" itself.  Ignoring the extra one.\n", def->cd_name);
		}
		else
		{
		    DBPlaceCell(use, cifReadCellDef);
		    madeinst = TRUE;
		}
	    }
	    else
	    {
		/* (To do:  Copy labels from flattened cells, with hierarchical	*/
		/* names.  Whether to do this or not should be an option.)	*/
		/* TxPrintf("Removing instances of flattened cell %s in %s\n",
			def->cd_name, cifReadCellDef->cd_name); */
		madeinst = TRUE;
	    }
	}
	else
	{
	    use = DBCellNewUse(def, (useid) ? useid : (char *) NULL);
	    if (isArray)
		DBMakeArray(use, &GeoIdentityTransform, xlo, ylo, xhi, yhi, xsep, ysep);
	    DBSetTrans(use, &trans);
	    if (DBCellFindDup(use, cifReadCellDef) != NULL)
	    {
		DBCellDeleteUse(use);
		CalmaReadError("Warning: cell \"%s\" placed on top of"
				" itself.  Ignoring the extra one.\n", def->cd_name);
	    }
	    else
	    {
		DBPlaceCell(use, cifReadCellDef);
		madeinst = TRUE;
	    }
	}
    }

    /* Done with allocated variables */
    if (sname != NULL) freeMagic(sname);
    if (useid != NULL) freeMagic(useid);
    if (arraystr != NULL) freeMagic(arraystr);

    return ((def->cd_flags & CDFLATGDS) ? (madeinst ? 1 : 0) : 1);
}


/* Callback function for determining if a cell has at least one subcell */

int
gdsHasUses(use, clientdata)
    CellUse *use;
    ClientData clientdata;
{
    return 1;
}

/* Callback function for copying paint from one CIF cell into another */

int
gdsCopyPaintFunc(tile, gdsCopyRec)
    Tile *tile;
    GDSCopyRec *gdsCopyRec;
{
    int pNum;
    TileType dinfo;
    Rect sourceRect, targetRect;
    Transform *trans = gdsCopyRec->trans;
    Plane *plane = gdsCopyRec->plane;

    dinfo = TiGetTypeExact(tile);

    if (trans)
    {
	TiToRect(tile, &sourceRect);
	GeoTransRect(trans, &sourceRect, &targetRect);
	if (IsSplit(tile))
	    dinfo = DBTransformDiagonal(TiGetTypeExact(tile), trans);
    }
    else
	TiToRect(tile, &targetRect);

    DBNMPaintPlane(plane, dinfo, &targetRect, CIFPaintTable,
		(PaintUndoInfo *)NULL);

    return 0;
}

/*
 * ----------------------------------------------------------------------------
 *
 * calmaUniqueCell --
 *
 *	Attempt to find a cell in the GDS subcell name hash table.
 *	If one exists, rename its definition so that it will not
 *	be overwritten when the cell is redefined.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------------
 */

void
calmaUniqueCell(sname)
    char *sname;
{
    HashEntry *h;
    CellDef *def, *testdef;
    char *newname;
    int snum = 0;

    h = HashLookOnly(&CifCellTable, sname);

    /* Existing entry with zero value indicates that the existing   */
    /* cell came from the same GDS file, so don't change anything.  */
    if ((h != NULL) && HashGetValue(h) == 0) return;

    def = DBCellLookDef(sname);
    if (def == (CellDef *)NULL)
	return;

    /* Cell may have been called but not yet defined---this is okay. */
    else if ((def->cd_flags & CDAVAILABLE) == 0)
        return;

    testdef = def;
    newname = (char *)mallocMagic(10 + strlen(sname));
     
    while (testdef != NULL)
    {
	/* Keep appending suffix indexes until we find one not used */
	sprintf(newname, "%s_%d", sname, ++snum);
	testdef = DBCellLookDef(newname);
    }
    DBCellRenameDef(def, newname);

    h = HashFind(&CifCellTable, (char *)sname);
    HashSetValue(h, 0);

    CalmaReadError("Warning: cell definition \"%s\" reused.\n", sname);
    freeMagic(newname);
}

/*
 * ----------------------------------------------------------------------------
 *
 * calmaFindCell --
 *
 * This local procedure is used to find a cell in the subcell table,
 * and create a new subcell if there isn't already one there.
 * If a new subcell is created, its CDAVAILABLE is left FALSE.
 *
 * Results:
 *	The return value is a pointer to the definition for the
 *	cell whose name is 'name'.
 *
 * Side effects:
 *	A new CellDef may be created.
 *
 * ----------------------------------------------------------------------------
 */

CellDef *
calmaFindCell(name, was_called, predefined)
    char *name;		/* Name of desired cell */
    bool *was_called;	/* If this cell is in the hash table, then it
			 * was instanced before it was defined.  We
			 * need to know this so as to avoid flattening
			 * the cell if requested.
			 */
    bool *predefined;	/* If this cell was in memory before the GDS
			 * file was read, then this flag gets set.
			 */

{
    HashEntry *h;
    CellDef *def;

    h = HashFind(&CifCellTable, name);
    if (HashGetValue(h) == 0)
    {
	def = DBCellLookDef(name);
	if (def == NULL)
	{
	    def = DBCellNewDef(name);

	    /*
	     * Tricky point:  call DBReComputeBbox here to make SURE
	     * that the cell has a valid bounding box.  Otherwise,
	     * if the cell is used in a parent before being defined
	     * then it will cause a core dump.
	     */
	     DBReComputeBbox(def);
	}
	else
	{
	    TxPrintf("Warning:  cell %s already existed before reading GDS!\n",
			name);
	    if (CalmaNoDuplicates)
	    {
		if (predefined) *predefined = TRUE;
	    	TxPrintf("Using pre-existing cell definition\n");
	    }
	}
	HashSetValue(h, def);
	if (was_called) *was_called = FALSE;
    }
    else
    {
	if (was_called)
	{
	    if (*was_called == TRUE)
	    {
		def = DBCellLookDef(name);
		if ((def != NULL) && (def->cd_flags & CDAVAILABLE))
	    	    if (CalmaNoDuplicates)
			if (predefined) *predefined = TRUE;
	    }
	    *was_called = TRUE;
	}
    }
    return (CellDef *) HashGetValue(h);
}

/*
 * ----------------------------------------------------------------------------
 *
 * calmaLookCell --
 *
 * This procedure is like calmaFindCell above, but it will not
 * create a new subcell if the named cell does not already exist.
 *
 * Results:
 *	The return value is a pointer to the definition for the
 *	cell whose name is 'name', or NULL if cell 'name' does
 *	not exist.
 *
 * Side effects:
 *	None.
 *
 * ----------------------------------------------------------------------------
 */

CellDef *
calmaLookCell(name)
    char *name;		/* Name of desired cell */
{
    HashEntry *h;

    h = HashLookOnly(&CifCellTable, name);
    if (h == NULL)
	return (CellDef *)NULL;
    else
	return (CellDef *)HashGetValue(h);
}

