#version 460
layout(location=0) noperspective out vec2 screen_ndc;
void main() {
    const vec2 corners[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    screen_ndc=corners[gl_VertexIndex];gl_Position=vec4(screen_ndc,0,1);
}
