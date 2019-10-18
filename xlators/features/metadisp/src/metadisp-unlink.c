
#include "metadisp.h"
#include <glusterfs/call-stub.h>

/**
 * The unlink flow in metadisp is complicated because we must
 * do ensure that UNLINK causes both the metadata objects
 * to get removed and the data objects to get removed.
 */

int32_t
metadisp_unlink_resume(call_frame_t *frame, xlator_t *this, loc_t *loc,
                       int xflag, dict_t *xdata)
{
    METADISP_TRACE("winding backend unlink to path %s", loc->path);
    STACK_WIND(frame, default_unlink_cbk, DATA_CHILD(this),
               DATA_CHILD(this)->fops->unlink, loc, xflag, xdata);
    return 0;
}

int32_t
metadisp_unlink_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                    int32_t op_ret, int32_t op_errno, struct iatt *preparent,
                    struct iatt *postparent, dict_t *xdata)
{
    call_stub_t *stub = NULL;
    if (cookie) {
        stub = cookie;
    }

    if (op_ret != 0) {
        goto unwind;
    }

    if (stub->poison) {
        call_stub_destroy(stub);
        stub = NULL;
        return 0;
    }

    call_resume(stub);
    return 0;

unwind:
    if (stub) {
        call_stub_destroy(stub);
    }
    STACK_UNWIND_STRICT(unlink, frame, op_ret, op_errno, preparent, postparent,
                        xdata);
    return 0;
}

int32_t
metadisp_unlink_lookup_cbk(call_frame_t *frame, void *cookie, xlator_t *this,
                           int32_t op_ret, int32_t op_errno, inode_t *inode,
                           struct iatt *buf, dict_t *xdata,
                           struct iatt *postparent)
{
    call_stub_t *stub = NULL;

    if (cookie) {
        stub = cookie;
    }

    if (op_ret != 0) {
        goto unwind;
    }

    // fail fast on empty gfid so we don't loop forever
    if (gf_uuid_is_null(buf->ia_gfid)) {
        op_ret = -1;
        op_errno = ENODATA;
        goto unwind;
    }

    // fill gfid since the stub is incomplete
    memcpy(stub->args.loc.gfid, buf->ia_gfid, sizeof(uuid_t));
    memcpy(stub->args.loc.pargfid, postparent->ia_gfid, sizeof(uuid_t));

    if (stub->poison) {
        call_stub_destroy(stub);
        stub = NULL;
        return 0;
    }

    call_resume(stub);
    return 0;

unwind:
    if (stub) {
        call_stub_destroy(stub);
    }
    STACK_UNWIND_STRICT(unlink, frame, op_ret, op_errno, NULL, NULL, NULL);
    return 0;
}

int32_t
metadisp_unlink(call_frame_t *frame, xlator_t *this, loc_t *loc, int xflag,
                dict_t *xdata)
{
    call_stub_t *stub = NULL;
    loc_t backend_loc = {
        0,
    };

    if (gf_uuid_is_null(loc->gfid)) {
        METADISP_TRACE("winding lookup for unlink to path %s", loc->path);

        // loop back to ourselves after a lookup
        stub = fop_unlink_stub(frame, metadisp_unlink, loc, xflag, xdata);
        STACK_WIND_COOKIE(frame, metadisp_unlink_lookup_cbk, stub,
                          METADATA_CHILD(this),
                          METADATA_CHILD(this)->fops->lookup, loc, xdata);
        return 0;
    }

    if (build_backend_loc(loc->gfid, loc, &backend_loc)) {
        goto unwind;
    }

    METADISP_TRACE("winding frontend unlink to path %s", loc->path);

    stub = fop_unlink_stub(frame, metadisp_unlink_resume, &backend_loc, xflag,
                           xdata);

    STACK_WIND_COOKIE(frame, metadisp_unlink_cbk, stub, METADATA_CHILD(this),
                      METADATA_CHILD(this)->fops->unlink, loc, xflag, xdata);
    return 0;
unwind:
    STACK_UNWIND_STRICT(unlink, frame, -1, EINVAL, NULL, NULL, NULL);
    return 0;
}
