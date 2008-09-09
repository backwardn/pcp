/*
 * Linux Filesystem Cluster
 *
 * Copyright (c) 2000,2004,2007-2008 Silicon Graphics, Inc.  All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 * 
 * Contact information: Silicon Graphics, Inc., 1500 Crittenden Lane,
 * Mountain View, CA 94043, USA, or: http://www.sgi.com
 */

#include "pmapi.h"
#include "impl.h"
#include "pmda.h"
#include "filesys.h"

static void 
refresh_filesys_projects(pmInDom qindom, filesys_t *fs)
{
    char 		buffer[MAXPATHLEN];
    project_t		*qp;
    fs_quota_stat_t	s;
    fs_disk_quota_t	d;
    size_t		idsz, devsz;
    FILE		*projects;
    char        	*p, *idend;
    uint32_t    	prid;
    int         	qcmd, sts;

    fs->flags &= ~(FSF_QUOT_PROJ_ACC | FSF_QUOT_PROJ_ENF);

    qcmd = QCMD(Q_XGETQSTAT, XQM_PRJQUOTA);
    if (quotactl(qcmd, fs->device, 0, (void*)&s) < 0)
	return;

    if (s.qs_flags & XFS_QUOTA_PDQ_ENFD)
	fs->flags |= FSF_QUOT_PROJ_ENF;
    if (s.qs_flags & XFS_QUOTA_PDQ_ACCT) 
	fs->flags |= FSF_QUOT_PROJ_ACC;
    else
	return;

    projects = fopen("/etc/projects", "r");
    if (projects == NULL)
	return; 

    qcmd = QCMD(Q_XQUOTASYNC, XQM_PRJQUOTA);
    quotactl(qcmd, fs->device, 0, NULL);

    while (fgets(buffer, sizeof(buffer), projects)) {
	if (buffer[0] == '#')
	    continue;

	prid = strtol(buffer, &idend, 10);
	idsz = idend - buffer;
	qcmd = QCMD(Q_XGETQUOTA, XQM_PRJQUOTA);
	if (!idsz || quotactl(qcmd, fs->device, prid, (void *)&d) < 0)
	    continue;

	devsz = strlen(fs->device);
	p = malloc(idsz+devsz+2);
	if (!p)
	    continue;
	memcpy(p, buffer, idsz);
	p[idsz] = ':'; 
	memcpy(&p[idsz+1], fs->device, devsz+1);

	qp = NULL;
	sts = pmdaCacheLookupName(qindom, p, NULL, (void **)&qp);
	if (sts == PMDA_CACHE_ACTIVE)	/* repeated line in /etc/projects? */
	    continue;
	if (sts != PMDA_CACHE_INACTIVE) {
	    qp = (project_t *)malloc(sizeof(project_t));
#if PCP_DEBUG
	    if (pmDebug & DBG_TRACE_LIBPMDA)
		fprintf(stderr, "refresh_filesys_projects: add \"%s\"\n", p);
#endif
	}
	qp->space_hard = d.d_blk_hardlimit;
	qp->space_soft = d.d_blk_softlimit;
	qp->space_used = d.d_bcount;
	qp->space_time_left = d.d_btimer;
	qp->files_hard = d.d_ino_hardlimit;
	qp->files_soft = d.d_ino_softlimit;
	qp->files_used = d.d_icount;
	qp->files_time_left = d.d_itimer;
	pmdaCacheStore(qindom, PMDA_CACHE_ADD, p, (void *)qp);
    }
    fclose(projects);
}

int
refresh_filesys(pmInDom filesys_indom, pmInDom quota_indom)
{
    char buf[MAXPATHLEN];
    char realdevice[MAXPATHLEN];
    filesys_t *fs;
    FILE *fp;
    char *path;
    char *device;
    char *type;
    int sts;

    pmdaCacheOp(quota_indom, PMDA_CACHE_INACTIVE);
    pmdaCacheOp(filesys_indom, PMDA_CACHE_INACTIVE);

    if ((fp = fopen("/proc/mounts", "r")) == (FILE *)NULL)
	return -errno;

    while (fgets(buf, sizeof(buf), fp) != NULL) {
	if (( device = strtok(buf, " ")) == 0
	   || strncmp(device, "/dev", 4) != 0)
	    continue;
	if (realpath(device, realdevice) != NULL)
	    device = realdevice;

	path = strtok(NULL, " ");
	type = strtok(NULL, " ");
	if (strcmp(type, "proc") == 0 ||
	    strcmp(type, "nfs") == 0 ||
	    strcmp(type, "devfs") == 0 ||
	    strcmp(type, "devpts") == 0 ||
	    strncmp(type, "auto", 4) == 0)
	    continue;

	sts = pmdaCacheLookupName(filesys_indom, device, NULL, (void **)&fs);
	if (sts == PMDA_CACHE_ACTIVE)	/* repeated line in /proc/mounts? */
	    continue;
	if (sts == PMDA_CACHE_INACTIVE) { /* re-activate an old mount */
	    pmdaCacheStore(filesys_indom, PMDA_CACHE_ADD, device, fs);
	    if (strcmp(path, fs->path) != 0) {	/* old device, new path */
		free(fs->path);
		fs->path = strdup(path);
	    }
	}
	else {	/* new mount */
	    if ((fs = malloc(sizeof(filesys_t))) == NULL)
		continue;
	    fs->device = strdup(device);
	    fs->path = strdup(path);
#if PCP_DEBUG
	    if (pmDebug & DBG_TRACE_LIBPMDA) {
		fprintf(stderr, "refresh_filesys: add \"%s\" \"%s\"\n",
		    fs->path, device);
	    }
#endif
	    pmdaCacheStore(filesys_indom, PMDA_CACHE_ADD, device, fs);
	}

	if (strcmp(type, "xfs") == 0)
	    refresh_filesys_projects(quota_indom, fs);
    }

    /*
     * success
     * Note: we do not call statfs() here since only some instances
     * may be requested (rather, we do it in linux_fetch, see pmda.c).
     */
    fclose(fp);
    return 0;
}
