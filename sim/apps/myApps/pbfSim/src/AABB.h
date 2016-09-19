/*******************************************************************************
 * AABB.h
 * - A Simple axis-aligned bounding box implementation
 *
 * CIS563: Physically Based Animation final project
 * Created by Michael Woods & Michael O'Meara
 ******************************************************************************/

#ifndef PBF_SIM_AABB_H
#define PBF_SIM_AABB_H

#include <iostream>
#include "MSAOpenCL.h"

/******************************************************************************/

class AABB
{
    protected:
        ofVec3f minV, maxV;

    public:
        AABB();
        AABB(float3 p, ofVec3f q);
        AABB(const AABB& other);
    
        ofVec3f& getMinExtent();
        ofVec3f& getMaxExtent();
    
        float width() const;
        float height() const;
        float depth() const;
    
        void addMin(ofVec3f amount);
        void addMax(ofVec3f amount);
    
        void addMinX(float amount);
        void addMaxX(float amount);
        void addMinY(float amount);
        void addMaxY(float amount);
        void addMinZ(float amount);
        void addMaxZ(float amount);
    
        friend std::ostream& operator<<(std::ostream& stream, const AABB& aabb);
};

/******************************************************************************/

#endif
