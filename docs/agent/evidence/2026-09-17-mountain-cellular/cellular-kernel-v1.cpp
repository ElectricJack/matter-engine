float surface_cellular3(float x, float y, float z, uint32_t seed, int feature) {
    if (!(x >= -16777216.f && x < 16777216.f &&
          y >= -16777216.f && y < 16777216.f &&
          z >= -16777216.f && z < 16777216.f) || feature < 0 || feature > 2) return 0;
    const int32_t ix=int32_t(std::floor(x)),iy=int32_t(std::floor(y)),iz=int32_t(std::floor(z));
    const float fx=x-float(ix),fy=y-float(iy),fz=z-float(iz);
    float first=100,second=100,value=0;
    // The bounded site jitter keeps both nearest sites in this 3x3x3 window:
    // own + the cell toward the nearest face are each at most sqrt(1.52)
    // away; every site outside the window is at least 1.3 away.
    // Relative coordinates avoid subtracting two large world positions.
    for(int dz=-1;dz<=1;++dz)for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
        const int32_t cx=ix+dx,cy=iy+dy,cz=iz+dz;
        const float px=float(dx)+.3f+.4f*rand01_3(cx,cy,cz,seed)-fx;
        const float py=float(dy)+.3f+.4f*rand01_3(cx,cy,cz,seed^0x9e37u)-fy;
        const float pz=float(dz)+.3f+.4f*rand01_3(cx,cy,cz,seed^0x7f4au)-fz;
        const float d=(px*px+py*py)+pz*pz;
        if(d<first) {second=first;first=d;value=rand01_3(cx,cy,cz,seed^0xa511e9b3u);}
        else if(d<second)second=d;
    }
    return feature==0?std::sqrt(first):feature==1?std::max(0.f,second-first):value;
}

