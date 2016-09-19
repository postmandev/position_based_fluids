/*******************************************************************************
 * Simulation.h
 * CIS563: Physically Based Animation final project
 * Created by Michael Woods & Michael O'Meara
 ******************************************************************************/

#ifndef PBF_SIM_SIMULATION_H
#define PBF_SIM_SIMULATION_H

#include <iostream>
#include <memory>
#include "Parameters.h"
#include "Constants.h"
#include "AABB.h"
#include "PrefixSum.h"
#include "MSAOpenCL.h"

/******************************************************************************/

// A particle type:

typedef struct {

    float4 pos;      // Current particle position (x)

    float4 posStar;  // Predicted particle position (x*)

    float4 vel;      // Current particle velocity (v)

    /**
     * VERY IMPORTANT: This is needed so that the struct's size is aligned 
     * for x86 memory access along 16 byte intervals.
     *
     * If the size is not aligned, results WILL be screwed up!!! 
     * Don't be like me and waste hours trying to debug this issue. The
     * OpenCL compiler WILL NOT pad your struct to so that boundary aligned
     * like g++/clang will in the C++ world.
     *
     * See http://en.wikipedia.org/wiki/Data_structure_alignment
     */
    //float4  __padding[1]; // Padding

} Particle;

// A type to represent the position of a given particle in the spatial
// grid the simulated world is divided into

typedef struct {

    int particleIndex; // Index of particle in particle buffer

    int cellI;         // Corresponding grid index in the x-axis

    int cellJ;         // Corresponding grid index in the y-axis

    int cellK;         // Corresponding grid index in the z-axis

    int key;           // Linearized index key computed from the subscript
                       // (cellI, cellJ, cellK)
    int __padding[3];
    
} ParticlePosition;

// A type that encodes the start and length of a grid cell in sortedParticleToCell

typedef struct {
    
    int  start; // Start of the grid cell in sortedParticleToCell
    
    int length;
    
    int __padding[2]; // Padding
    
} GridCellOffset;

/******************************************************************************/

/**
 * This class encompasses the current statue of the 
 * Position-Based Fluids/Dynamics system at a given point in time. Much of the
 * code defining the implementation of this class was originally derived
 * from the second assignment in the class, which in turn, was based off of
 * Matthias Muller's "Position Based Dynamics" paper
 */
class Simulation
{
    public:
        enum AnimationType
        {
            SINE_WAVE
           ,LINEAR_RAMP
           ,COMPRESS
        };
    
    private:
        // Count of the current frame number
        unsigned int frameNumber;
    
        // Total number of cells in the system
        int numCells;
    
        // Flag to draw the spatial grid
        bool doDrawGrid;
    
        // Flag for visual debugging
        bool doVisualDebugging;
    
        // Flag to indicate that the bounds of the simulation should be
        // animated, e.g. moving in some periodic fashion
        bool animBounds;

        // Counter used for the current bounds-modifying animation operation
        unsigned int animFrameNumber;
    
        // The animation type for animating the bounds of the simulation
        AnimationType animType;
    
        // Bounds animation period
        float animPeriod;
    
        // Bounds animation amplitude
        float animAmp;
    
        // Flag to toggle bounds animation on both sides of the simulation area
        bool animBothSides;

        // Given a particle count, particle radius and world bounds,
        // find the "ideal" cell count per axis
        ofVec3f findIdealParticleCount();

        // Moves data from GPU buffers back to the host
        void readFromGPU();
    
        // Writes data from the host to buffers on the GPU (device)
        void writeToGPU();

    protected:
        // Particle mesh sphere
        ofMesh particleMesh;
    
        // Particle vertices
        ofVbo particleVertices;
    
        // OpenCL manager
        msa::OpenCL& openCL;
    
        // Used to compute the prefix sum of the cell histogram array. This
        // is needed in order to sort the particles by grid cell so fast,
        // fixed-radius particle neighbor lookup is possible
        std::shared_ptr<PrefixSum> prefixSum;
    
        // Basic shader
        ofShader shader;
    
        // Bounding volume
        AABB originalBounds; // Starting, unmodified bounds
        AABB bounds;         // Modifiable bounds
    
        // Timestep size:
        float dt;
    
        // Cells per axis for spatial subdivision:
        ofVec3f cellsPerAxis;

        // Total number of particles in the system
        int numParticles;

        // Simulation parameters to pass to the kernels
        Parameters parameters;
 
        // And the buffer to hold the parameters on the GPU:
        msa::OpenCLBuffer parameterBuffer;
    
        // All particles in the simulation
        msa::OpenCLBufferManagedT<Particle>	particles;
    
        // An array of particle-to-cell mappings
        // - Buffer of ParticlePosition
        msa::OpenCLBuffer particleToCell;
    
        // A cell count histogram used for particle neighbor finding
        // - Buffer of ints
        msa::OpenCLBuffer cellHistogram;
    
        // Prefix sums computed from the cell histogram array
        // - Buffer of ints
        msa::OpenCLBuffer cellPrefixSums;

        // A sorted version of particleToCell, used to search for a given
        // particle's neighbors
        // - Buffer of ParticlePosition
        msa::OpenCLBuffer sortedParticleToCell;

        // An array of cell start locations and spans in sortedParticleToCell
        // - Buffer of GridCellOffset
        msa::OpenCLBuffer gridCellOffsets;
    
        // Particle densities computed by SPH estimation
        // - Buffer of float
        msa::OpenCLBuffer density;

        // Particle density lambda value from the section "Enforcing
        // Incompressibility" of "Position Based Fluids"
        // - Buffer of float1
        msa::OpenCLBuffer lambda;
    
        // Vorticity curl force applied to each particle
        // - Buffer of float4
        msa::OpenCLBuffer curl;
    
        // Accumulated external forces
        // - Buffer of float4
        msa::OpenCLBuffer extForces;
    
        // Position deltas
        msa::OpenCLBuffer posDelta;

        // Final render position for OpenCL <-> OpenGL instanced rendering
        msa::OpenCLBufferManagedT<float4> renderPos;

        // Initialization-related functions:
        void initialize();
        void initializeBuffers();
        void setupKernels(bool load);
        void initializeOpenGL();

        // Particle sorting functions:
        void discretizeParticlePositions();
        void sortParticlesByCell();
    
        // Simulation state-related functions:
        void resetQuantities();
        void predictPositions();
        void findNeighboringParticles();
        void calculateDensity();
        void calculatePositionDelta();
        void updatePositionDelta();
        void handleCollisions();
        void updatePosition();
    
        // Drawing-related functions:
        void drawBounds(const ofCamera& camera);
        void drawGrid(const ofCamera& camera);
        void drawParticles(const ofCamera& camera);

        void stepBoundsAnimation();
    
    public:
        Simulation(msa::OpenCL& openCL
                  ,AABB bounds
                  ,int numParticles
                  ,Parameters parameters);
    
        Simulation(msa::OpenCL& openCL
                  ,AABB bounds
                  ,int numParticles
                  ,float dt
                  ,ofVec3f cellsPerAxis
                  ,Parameters parameters);

        virtual ~Simulation();

        const unsigned int getFrameNumber() const { return this->frameNumber; }

        const AABB& getBounds() const { return this->bounds; }
        void setBounds(const AABB& bounds) { this->bounds = bounds; }

        const ofVec3f& getCellsPerAxis() const { return this->cellsPerAxis; }
    
        const unsigned int getNumberOfParticles() const { return this->numParticles; }
    
        const unsigned int getNumberOfCells() const { return this->numCells; }
    
        const Parameters& getParameters() const;
        void setParameters(const Parameters& parameters);
    
        const bool drawGridEnabled() const { return this->doDrawGrid; }
        void toggleDrawGrid()              { this->doDrawGrid = !this->doDrawGrid; }
    
        const bool isVisualDebuggingEnabled() const { return this->doVisualDebugging; }
        void toggleVisualDebugging()                { this->doVisualDebugging = !this->doVisualDebugging; }

        void enableBoundsAnimation()     { this->animBounds = true; }
        void disableBoundsAnimation()    { this->animBounds = false; }

        void enableBothSidesAnimation()  { this->animBothSides = true; }
        void disableBothSidesAnimation() { this->animBothSides = false; }
    
        void setAnimationType(AnimationType animType) { this->animType = animType; }
        void setAnimationPeriod(float period)         { this->animPeriod = period; }
        void setAnimationAmp(float amp)               { this->animAmp = amp; }
    
        void reset();
        void step();
        void resetBounds();
        void draw(const ofCamera& camera);
};

#endif
