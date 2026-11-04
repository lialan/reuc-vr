#pragma once
// Intersect the controller ray with the scene's depth-writing triangles. Work
// in guest view space directly: clip depth loses precision at close range.
// Only candidate meshes reach the triangle tests; no GPU readback or wait.
#include "gx_vertices.h"
#include "../xr/stereo_scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace uc {
class AimGeometry {
    using Point = std::array<float,3>;
    struct Ray { Point origin{}, direction{}; float nearest=0; bool valid=false; };
    std::array<Ray,2> rays_{};
    std::vector<Point> points_;
    static Point sub(const Point& a,const Point& b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
    static Point cross(const Point& a,const Point& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
    static double dot(const Point& a,const Point& b) { return double(a[0])*b[0]+double(a[1])*b[1]+double(a[2])*b[2]; }
    static bool box_hit(const Ray& ray,const Point& lo,const Point& hi) {
        double first=0,last=ray.nearest;
        for (int i=0;i<3;++i) {
            if (std::abs(ray.direction[i])<1e-8f) { if (ray.origin[i]<lo[i] || ray.origin[i]>hi[i]) return false; }
            else {
                double a=(lo[i]-ray.origin[i])/ray.direction[i], b=(hi[i]-ray.origin[i])/ray.direction[i];
                if (a>b) std::swap(a,b);
                first=std::max(first,a); last=std::min(last,b);
                if (first>last) return false;
            }
        }
        return last>0;
    }
public:
    void reset() { rays_={}; }
    void set_ray(unsigned hand,const xr::StereoPose& pose,float scale) {
        auto& ray=rays_.at(hand); ray={};
        const float forward[]={0,0,-1};
        lg_stereo_rotate(pose.orientation.data(),forward,ray.direction.data());
        for (int i=0;i<3;++i) {
            ray.origin[i]=pose.position[i]*scale;
            if (!std::isfinite(ray.origin[i]) || !std::isfinite(ray.direction[i])) return;
        }
        const double norm=std::sqrt(dot(ray.direction,ray.direction));
        if (norm<1e-6) return;
        for (float& d:ray.direction) d/=float(norm);
        ray.nearest=std::numeric_limits<float>::infinity(); ray.valid=true;
    }
    bool active() const { return rays_[0].valid || rays_[1].valid; }
    float depth(unsigned hand,float scale) const {
        const auto& ray=rays_.at(hand);
        if (!ray.valid || !std::isfinite(ray.nearest)) return 0;
        return -(ray.origin[2]+ray.nearest*ray.direction[2])/scale;
    }
    void draw(const gx::DrawPacket& draw,const std::array<float,6>* box=nullptr) {
        if (!active() || draw.primitive>=0xA8) return;
        const auto vertices=draw.vertex_view(); const auto indices=draw.index_view();
        if (vertices.empty() || indices.size()%3) return;
        const auto& t=*draw.transforms;
        const bool indexed=draw.state->descriptors[0]!=gx::Mode::none;
        const auto transform=[&](const Point& p,unsigned row) {
            Point v{};
            if (row>61) return Point{NAN,NAN,NAN};
            for (int i=0;i<3;++i) { const auto& m=t.position_rows[row+i]; v[i]=m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3]; }
            return v;
        };
        const auto bounds=[&](const auto& points) {
            Point lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
            for (const auto& p:points) for (int i=0;i<3;++i) { lo[i]=std::min(lo[i],p[i]); hi[i]=std::max(hi[i],p[i]); }
            return std::array<bool,2>{rays_[0].valid && box_hit(rays_[0],lo,hi), rays_[1].valid && box_hit(rays_[1],lo,hi)};
        };
        if (box && !indexed) {
            std::array<Point,8> corners;
            for (int i=0;i<8;++i) corners[i]=transform({(*box)[i&1 ? 3:0],(*box)[i&2 ? 4:1],(*box)[i&4 ? 5:2]},t.current);
            const auto hit=bounds(corners); if (!hit[0] && !hit[1]) return;
        }
        points_.clear(); points_.reserve(vertices.size());
        for (const auto& v:vertices) points_.push_back(transform(v.position,indexed?v.position_matrix:t.current));
        const auto hit=bounds(points_); if (!hit[0] && !hit[1]) return;
        for (size_t i=0;i<indices.size();i+=3) {
            if (indices[i]>=points_.size() || indices[i+1]>=points_.size() || indices[i+2]>=points_.size()) continue;
            const auto& a=points_[indices[i]];
            const auto e1=sub(points_[indices[i+1]],a),e2=sub(points_[indices[i+2]],a);
            for (unsigned hand=0;hand<2;++hand) if (hit[hand]) {
                auto& ray=rays_[hand]; const auto p=cross(ray.direction,e2);
                const double det=dot(e1,p); if (std::abs(det)<1e-10) continue;
                const auto s=sub(ray.origin,a); const double u=dot(s,p)/det;
                if (!(u>=0 && u<=1)) continue;
                const auto q=cross(s,e1); const double v=dot(ray.direction,q)/det;
                if (!(v>=0 && u+v<=1)) continue;
                const double reach=dot(e2,q)/det;
                if (reach>1e-4 && reach<ray.nearest) ray.nearest=float(reach);
            }
        }
    }
};
}
