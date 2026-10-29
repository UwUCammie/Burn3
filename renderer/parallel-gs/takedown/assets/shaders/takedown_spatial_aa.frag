#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 FragColor;
layout(set=0,binding=0) uniform sampler2D World;
layout(push_constant) uniform Parameters { vec2 inverse_extent; uint taps; uint unused; } params;
float luma(vec3 c) { return dot(c, vec3(0.299,0.587,0.114)); }
void main() {
    vec2 px = params.inverse_extent;
    vec4 center = textureLod(World, vUV, 0.0);
    if (params.taps == 0) { FragColor=center; return; }
    float m=luma(center.rgb);
    float n=luma(textureLod(World,vUV+vec2(0,-px.y),0).rgb);
    float s=luma(textureLod(World,vUV+vec2(0, px.y),0).rgb);
    float w=luma(textureLod(World,vUV+vec2(-px.x,0),0).rgb);
    float e=luma(textureLod(World,vUV+vec2( px.x,0),0).rgb);
    float hi=max(m,max(max(n,s),max(e,w))), lo=min(m,min(min(n,s),min(e,w)));
    if (hi-lo < max(0.03125,hi*0.166)) { FragColor=center; return; }
    // Follow the local edge, preserving flat areas and avoiding a whole-frame
    // blur. All samples belong to this frame: there is no temporal history.
    float nw=luma(textureLod(World,vUV-px,0).rgb);
    float ne=luma(textureLod(World,vUV+vec2(px.x,-px.y),0).rgb);
    float sw=luma(textureLod(World,vUV+vec2(-px.x,px.y),0).rgb);
    float se=luma(textureLod(World,vUV+px,0).rgb);
    vec2 gradient=vec2((ne+2*e+se)-(nw+2*w+sw),(sw+2*s+se)-(nw+2*n+ne));
    vec2 edge=vec2(-gradient.y,gradient.x);
    float length=max(abs(edge.x),abs(edge.y));
    if (length<0.0001) { FragColor=center; return; }
    edge=edge/length*px;
    vec3 sum=vec3(0); float total=0;
    uint count=clamp(params.taps,2u,16u);
    for (uint i=0;i<count;++i) {
        float offset=(float(i)+0.5)/float(count)-0.5;
        vec3 sample_color=textureLod(World,vUV+edge*offset*1.5,0).rgb;
        // Bilinear taps stay bounded by the sampled colours. Preserve RGB
        // chroma instead of clamping channels to a luminance interval.
        sum+=sample_color; total+=1.0;
    }
    FragColor=vec4(sum/total,center.a);
}
