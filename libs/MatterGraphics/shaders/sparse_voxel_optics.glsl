// Shared primary/shadow coverage law in normalized cell coordinates.
struct SparseBrick { ivec4 coord; uvec4 cells; }; // coord.w: occupied bounds; cells: mask lo/hi, first cell, pad
struct SparseCell {
    vec4 color_density; // linear RGB; area density or direct planar coverage
    vec4 mean;          // w: packed support x min/max
    vec4 moment;        // projected-area ellipsoid xx yy zz xy
    vec4 cross_moment;  // xz yz, packed support y/z min/max
    vec4 plane;        // optional unit normal, offset relative to address cell
};

// The bake fits measured mean absolute projections, rather than substituting
// RMS normal projection. Legacy fixtures retain their earlier moment estimate.
float sparse_projected_area(SparseCell c, vec3 d) {
    return sqrt(max(0.0, dot(c.moment.xyz, d*d) +
        2.0*(c.moment.w*d.x*d.y + c.cross_moment.x*d.x*d.z +
             c.cross_moment.y*d.y*d.z)));
}

bool sparse_slab(vec3 ro, vec3 rd, vec3 lo, vec3 hi, out float enter, out float leave) {
    enter=0.0; leave=1e30;
    for (int a=0; a<3; ++a) {
        if (abs(rd[a]) < 1e-10) {
            if (ro[a] < lo[a] || ro[a] >= hi[a]) return false;
        } else {
            float t0=(lo[a]-ro[a])/rd[a], t1=(hi[a]-ro[a])/rd[a];
            enter=max(enter,min(t0,t1)); leave=min(leave,max(t0,t1));
        }
    }
    return leave > enter;
}


float sparse_cell_optical(SparseCell c,vec3 cell_ro,vec3 rd,float t,float next,float remaining,out float hit) {
            vec2 bx=unpackUnorm2x16(floatBitsToUint(c.mean.w));
            vec2 by=unpackUnorm2x16(floatBitsToUint(c.cross_moment.z));
            vec2 bz=unpackUnorm2x16(floatBitsToUint(c.cross_moment.w));
            vec3 support_lo=vec3(bx.x,by.x,bz.x),support_hi=vec3(bx.y,by.y,bz.y);
            float bound_enter,bound_leave;
            bool intersects=sparse_slab(cell_ro,rd,support_lo,support_hi,bound_enter,bound_leave);
            float optical=0.0,extinction=0.0;hit=0.0;
            bool planar=dot(c.plane.xyz,c.plane.xyz)>0.5;
            if(planar) {
                float denominator=dot(c.plane.xyz,rd);
                if(abs(denominator)>1e-8) {
                    hit=(c.plane.w-dot(c.plane.xyz,cell_ro))/denominator;
                    // The support slab can be thinner than a distant ray's
                    // float ULP. Intersect the plane directly, then test its
                    // point; an empty rounded slab is not a missing surface.
                    float epsilon=max(1e-6,abs(hit)*4.7683716e-7);
                    vec3 point=cell_ro+rd*hit;
                    if(hit>=t-epsilon && hit<=next+epsilon &&
                        all(greaterThanEqual(point,support_lo-vec3(epsilon))) &&
                        all(lessThanEqual(point,support_hi+vec3(epsilon))))
                        optical=c.color_density.w>=1.0?1e30:-log(1.0-c.color_density.w);
                }
            } else if(intersects) {
                vec3 extent=support_hi-support_lo;
                extinction=c.color_density.w*sparse_projected_area(c,rd)/(extent.x*extent.y*extent.z);
                float start=max(t,bound_enter),finish=min(next,bound_leave);
                optical=extinction*max(0.0,finish-start);
                hit=start+remaining/max(extinction,1e-30);
            }
    return optical;
}
