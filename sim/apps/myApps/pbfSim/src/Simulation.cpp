/*******************************************************************************
 * Simulation.cpp
 * - The heart of the position-based fluids simulator. This class encapsulates
 *   the current state of the simulation
 *
 * CIS563: Physically Based Animation final project
 * Created by Michael Woods & Michael O'Meara
 ******************************************************************************/

#define _USE_MATH_DEFINES
#include <cmath>
#include "ofMain.h"
#include "Constants.h"
#include "Simulation.h"

/******************************************************************************/

using namespace std;

/******************************************************************************/

ostream& operator<<(ostream& os, Particle p)
{
    return os << "Particle {" << endl
              << "  pos: <" << p.pos.x << "," << p.pos.y << "," << p.pos.z << ">" << endl
              << "  vel: <" << p.vel.x << "," << p.vel.y << "," << p.vel.z << ">" << endl
              << "}";
}

/******************************************************************************/

/**
 * Constructs a new simulation instance
 *
 * @param [in] _openCL OpenCL manager instance
 * @param [in] _bounds Defines the boundaries of the simulation in world space
 * @param [in] _numParticles The number of particles in the simulation
 * @param [in] _parameters Simulation parameters
 */
Simulation::Simulation(msa::OpenCL& _openCL
                      ,AABB _bounds
                      ,int _numParticles
                      ,Parameters _parameters) :
    openCL(_openCL),
    bounds(_bounds),
    originalBounds(_bounds),
    numParticles(_numParticles),
    dt(Constants::DEFAULT_DT),
    parameters(_parameters),
    frameNumber(0),
    animFrameNumber(0),
    animBounds(false),
    animType(SINE_WAVE),
    animPeriod(1.0f),
    animAmp(10.0f),
    animBothSides(false),
    doDrawGrid(false),
    doVisualDebugging(false)
{
    // Given the number of particles, find the ideal number of cells per axis
    // such that no cell contains more than 4 particles
    
    this->cellsPerAxis = this->findIdealParticleCount();
    
    this->initialize();
}

/**
 * Constructs a new simulation instance
 *
 * @param [in] _openCL OpenCL manager instance
 * @param [in] _bounds Defines the boundaries of the simulation in world space
 * @param [in] _numParticles The number of particles in the simulation
 * @param [in] _dt The time step (usually 1/30)
 * @param [in] _cellsPerAxis Cell spatial grid subdivisions per axis
 * @param [in] _parameters Simulation parameters
 */
Simulation::Simulation(msa::OpenCL& _openCL
                      ,AABB _bounds
                      ,int _numParticles
                      ,float _dt
                      ,ofVec3f _cellsPerAxis
                      ,Parameters _parameters) :
    openCL(_openCL),
    bounds(_bounds),
    originalBounds(_bounds),
    numParticles(_numParticles),
    dt(_dt),
    cellsPerAxis(_cellsPerAxis),
    parameters(_parameters),
    frameNumber(0),
    animFrameNumber(0),
    animBounds(false),
    animType(SINE_WAVE),
    animPeriod(1.0f),
    animAmp(10.0f),
    animBothSides(false),
    doDrawGrid(false),
    doVisualDebugging(false)
{
    this->initialize();
}

Simulation::~Simulation()
{
    
}

/******************************************************************************/

/**
 * Finds the ideal number of cells per axis so that the number of
 * particles that need to be searched is minimized
 */
ofVec3f Simulation::findIdealParticleCount()
{
    auto minExt   = this->bounds.getMinExtent();
    auto maxExt   = this->bounds.getMaxExtent();
    
    float width   = maxExt.x - minExt.x;
    float height  = maxExt.y - minExt.y;
    float depth   = maxExt.z - minExt.z;
    float radius  = this->parameters.particleRadius;
    float subDivX = static_cast<float>(Constants::PARTICLES_PER_CELL_X);
    float subDivY = static_cast<float>(Constants::PARTICLES_PER_CELL_Y);
    float subDivZ = static_cast<float>(Constants::PARTICLES_PER_CELL_Z);
    
    int cellsX   = static_cast<int>(ceil((width / radius) / subDivX));
    int cellsY   = static_cast<int>(ceil((height / radius) / subDivY));
    int cellsZ   = static_cast<int>(ceil((depth / radius) / subDivZ));
    
    return ofVec3f(cellsX, cellsY, cellsZ);
}

/**
 * Moves data from GPU buffers back to the host
 */
void Simulation::readFromGPU()
{
    this->parameterBuffer.read(&this->parameters, 0, sizeof(Parameters));
    this->particles.readFromDevice();
    this->renderPos.readFromDevice();
}

/**
 * Writes data from the host to buffers on the GPU (i.e. the "device" in
 * OpenCL parlance)
 */
void Simulation::writeToGPU()
{
    this->parameterBuffer.write(&this->parameters, 0, sizeof(Parameters));
    this->particles.writeToDevice();
    this->renderPos.writeToDevice();
}

/**
 * Returns the current simulation parameters
 */
const Parameters& Simulation::getParameters() const
{
    return this->parameters;
}

/**
 * Sets the current simulation parameters, writing the new parameters to the GPU
 *
 * @param [in] parameters The new parameters
 */
void Simulation::setParameters(const Parameters& parameters)
{
    this->parameters = parameters;
    this->parameterBuffer.write(&this->parameters, 0, sizeof(Parameters));
}

/******************************************************************************/

/**
 * Allocates the neceddary OpenCL buffers used in the simulation and sets the
 * state accordingly
 */
void Simulation::initializeBuffers()
{
    auto p1 = this->bounds.getMinExtent();
    auto p2 = this->bounds.getMaxExtent();
    
    // Initialize a buffer to hold dynamic simulation related parameters:
    this->parameterBuffer.initBuffer(sizeof(Parameters));
    
    this->numCells =   static_cast<int>(this->cellsPerAxis.x)
                     * static_cast<int>(this->cellsPerAxis.y)
                     * static_cast<int>(this->cellsPerAxis.z);

    // Dimension the OpenCL buffer to hold the given number of particles and
    // the render positions
    
    this->particles.initBuffer(this->numParticles);
    
#ifdef DRAW_PARTICLES_AS_SPHERES
    this->renderPos.initBuffer(this->numParticles);
#else
    this->renderPos.initFromGLObject(this->particleVertices.getVertId(), this->numParticles);
#endif
    
    // Accumulated forces acting on the i-th particles
    
    this->extForces.initBuffer(this->numParticles * sizeof(float4));
    
    // particleToCell contains [0 .. this->numParticles - 1] entries, where
    // each ParticlePosition instance (index is not important) maps
    // a particle's index (ParticlePosition#particleIndex) to a spatial grid
    // cell (ParticlePosition#cellI, ParticlePosition#cellJ, ParticlePosition#cellK),
    // where 0 <= ParticlePosition#cellI < cellsPerAxis.x,
    // 0 <= ParticlePosition#cellJ < cellsPerAxis.y, and
    // 0 <= ParticlePosition#cellK < cellsPerAxis.z
    
    this->particleToCell.initBuffer(this->numParticles * sizeof(ParticlePosition));
    
    // Where the sorted version of the above will be sorted per simulation
    // step. The ParticlePosition indices will be sorted in ascending order
    // according to the linearized index computed from
    // (ParticlePosition#cellI, ParticlePosition#cellJ, ParticlePosition#cellK
    //
    // See the kernel helper function sub2ind in kernels/Simulation.cl for
    // details
    
    this->sortedParticleToCell.initBuffer(this->numParticles * sizeof(ParticlePosition));
    
    // An array containing [0 .. this->numCells - 1] entries, where the
    // i-th entry contains the offset information about the start of a
    // particular grid cell in sortedParticleToCell. gridCellOffsets entries
    // are structs of type GridCellOffset, and are considered valid if
    // GridCellOffset#start != -1. This is used to speed up the lookup for
    // particles that happen to be in the same cell, so for instance, given
    // a grid cell offset at index i, g_i, all of the particles in cell
    // i are in the range sortedParticleToCell[g_i.start .. (g_i.start + g_i.length)]
    
    this->gridCellOffsets.initBuffer(this->numCells * sizeof(GridCellOffset));
    
    // A histogram (count table), where the i-th entry contains the number of
    // particles occupying that linearized grid cell. For a linear grid cell
    // z, z can be computed from subscripts (i, j, k) by way of
    // z = i + (j * GRIDWIDTH) + (k * GRIDWIDTH * GRIDHEIGHT)
    
    this->cellHistogram.initBuffer(this->numCells * sizeof(int));
    
    // TODO
    this->cellPrefixSums.initBuffer(this->numCells * sizeof(int));
    
    // The density/lambda/vorticity curl force values associated with each
    // particle. The i-th density corresponds to the i-th quantitity in each
    // buffer:
    
    this->density.initBuffer(this->numParticles * sizeof(float));
    this->lambda.initBuffer(this->numParticles * sizeof(float));
    this->curl.initBuffer(this->numParticles * sizeof(float4));
    
    // For particle position correction in the solver:
    
    this->posDelta.initBuffer(this->numParticles * sizeof(float4));
    
    // Set up initial positions and velocities for the particles:
    
    float radius = this->parameters.particleRadius;
    
    for (int i = 0; i < this->numParticles; i++) {
        
        Particle &p = this->particles[i];
        
        // Random position in the bounding box:
        p.pos.x = ofRandom(p1.x + radius, p2.x - radius);
        p.pos.y = ofRandom(p1.y + radius, 0.25f * (p2.y - radius));
        //p.pos.y = ofRandom(p1.y + radius, p2.y - radius);
        p.pos.z = ofRandom(p1.z + radius, p2.z - radius);
        
        // No predicted position:
        p.posStar.x = p.posStar.y = p.posStar.z = 0.0f;
        
        // and no initial velocity:
        p.vel.x = p.vel.y = p.vel.z = 0.0f;
    }
}

/**
 * Initializes the simulation state and loads the requisite OpenCL kernels
 */
void Simulation::initialize()
{
    // Set up OpenGL VBOs and shader programs:
    
    this->initializeOpenGL();
    
    // Allocate OpenCL buffers and set the initial state values:
    
    this->initializeBuffers();
    
    // Setup the kernels. If true is given, the kernels will be loaded in
    // addition to arguments being bound. If false, only arguments will be
    // bound:

    this->setupKernels(true);

    // Finally, dump the initial quantities assigned to the particles to the
    // GPU, so we can use them in GPU-land/OpenCL

    this->writeToGPU();
}

/**
 * Initializes all shared OpenGL objects used in the simulation
 */
void Simulation::initializeOpenGL()
{
    // Set up the particle geometry for instancing:
    
    this->particleMesh = ofMesh::sphere(this->parameters.particleRadius);
    
    // Set up the shaders:
    
    string shaderType;

#ifdef DRAW_PARTICLES_AS_SPHERES
    shaderType = "SphereParticle";
#else
    shaderType = "PointParticle";
#endif
    
    bool loadStatus = this->shader.load("shaders/" + shaderType + ".vert"
                                       ,"shaders/" + shaderType + ".frag");

    if (loadStatus) {
        ofLogNotice() << ("Loaded shader: " + shaderType) << endl;
    } else {
        ofLogError() << ("Failed to load shader: " + shaderType) << endl;
    }
    
    // Bind default symbols:

    this->shader.bindDefaults();
    
    // Add zero'd vertex data for the VBO which will be updated from
    // OpenCL:
    
#ifdef DRAW_PARTICLES_AS_SPHERES

    // Nothing to do

#else

    // Set up how our particles are going to be displayed as points:
    glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
    
    this->particleVertices.setVertexData((const float*)0     // No need to explicitly upload anything, since it'll be zeros anyway
                                        ,4                   // Our points are represented by a 4D homogenous point (x,y,z,w)
                                        ,this->numParticles
                                        ,GL_STATIC_DRAW
                                        ,sizeof(float) * 4); // Each point is 4 floats
    
    // Copy the normal data from the sphere to fake a spherical shape
    // in the shaders later:
    
    this->particleVertices.setNormalData(this->particleMesh.getNormalsPointer()
                                        ,this->numParticles
                                        ,GL_STATIC_DRAW);
    
#endif
}

/**
 * Loads all of the OpenCL kernels that will be used for during the simulation
 * If true is given for load, the kernels will be loaded in addition to 
 * arguments being bound. If false, only arguments will be bound
 */
void Simulation::setupKernels(bool load)
{
    auto minExt = this->bounds.getMinExtent();
    auto maxExt = this->bounds.getMaxExtent();

    int  cellsX = static_cast<int>(this->cellsPerAxis.x);
    int  cellsY = static_cast<int>(this->cellsPerAxis.y);
    int  cellsZ = static_cast<int>(this->cellsPerAxis.z);
    
    // Read the source files for the kernels:

    // === Simulation.cl : the basis for the PBF simulation ====================
    
    if (load) {
        this->openCL.loadProgramFromFile("kernels/Simulation.cl");
    }

    // KERNEL :: debugHistogram
    
    if (load) {
        this->openCL.loadKernel("debugHistogram");
    }
    this->openCL.kernel("debugHistogram")->setArg(0, this->cellHistogram);
    this->openCL.kernel("debugHistogram")->setArg(1, this->cellPrefixSums);
    this->openCL.kernel("debugHistogram")->setArg(2, this->numCells);
    
    // KERNEL :: debugSorting

    if (load) {
        this->openCL.loadKernel("debugSorting");
    }
    this->openCL.kernel("debugSorting")->setArg(0, this->particleToCell);
    this->openCL.kernel("debugSorting")->setArg(1, this->sortedParticleToCell);
    this->openCL.kernel("debugSorting")->setArg(2, this->numParticles);

    // KERNEL :: resetParticleQuantities

    if (load) {
        this->openCL.loadKernel("resetParticleQuantities");
    }
    this->openCL.kernel("resetParticleQuantities")->setArg(0, this->particles);
    this->openCL.kernel("resetParticleQuantities")->setArg(1, this->particleToCell);
    this->openCL.kernel("resetParticleQuantities")->setArg(2, this->sortedParticleToCell);
    this->openCL.kernel("resetParticleQuantities")->setArg(3, this->density);
    this->openCL.kernel("resetParticleQuantities")->setArg(4, this->lambda);
    this->openCL.kernel("resetParticleQuantities")->setArg(5, this->posDelta);
    
    // KERNEL :: resetCellQuantities

    if (load) {
        this->openCL.loadKernel("resetCellQuantities");
    }
    this->openCL.kernel("resetCellQuantities")->setArg(0, this->cellHistogram);
    this->openCL.kernel("resetCellQuantities")->setArg(1, this->cellPrefixSums);
    this->openCL.kernel("resetCellQuantities")->setArg(2, this->gridCellOffsets);

    // KERNEL :: predictPosition

    if (load) {
        this->openCL.loadKernel("predictPosition");
    }
    this->openCL.kernel("predictPosition")->setArg(0, this->particles);
    this->openCL.kernel("predictPosition")->setArg(1, this->extForces);
    this->openCL.kernel("predictPosition")->setArg(2, this->dt);
    
    // KERNEL :: discretizeParticlePositions

    if (load) {
        this->openCL.loadKernel("discretizeParticlePositions");
    }
    this->openCL.kernel("discretizeParticlePositions")->setArg(0, this->particles);
    this->openCL.kernel("discretizeParticlePositions")->setArg(1, this->particleToCell);
    this->openCL.kernel("discretizeParticlePositions")->setArg(2, this->cellHistogram);
    this->openCL.kernel("discretizeParticlePositions")->setArg(3, cellsX);
    this->openCL.kernel("discretizeParticlePositions")->setArg(4, cellsY);
    this->openCL.kernel("discretizeParticlePositions")->setArg(5, cellsZ);
    this->openCL.kernel("discretizeParticlePositions")->setArg(6, minExt);
    this->openCL.kernel("discretizeParticlePositions")->setArg(7, maxExt);

    // KERNEL :: countSortParticlesByCell
    
    if (load) {
        this->openCL.loadKernel("countSortParticlesByCell");
    }
    this->openCL.kernel("countSortParticlesByCell")->setArg(0, this->particleToCell);
    this->openCL.kernel("countSortParticlesByCell")->setArg(1, this->sortedParticleToCell);
    this->openCL.kernel("countSortParticlesByCell")->setArg(2, this->cellPrefixSums);
    this->openCL.kernel("countSortParticlesByCell")->setArg(3, this->numParticles);
    
    // KERNEL :: findParticleBins
    
    if (load) {
        this->openCL.loadKernel("findParticleBins");
    }
    this->openCL.kernel("findParticleBins")->setArg(0, this->sortedParticleToCell);
    this->openCL.kernel("findParticleBins")->setArg(1, this->gridCellOffsets);
    this->openCL.kernel("findParticleBins")->setArg(2, this->numParticles);
    
    // KERNEL :: estimateDensity

    if (load) {
        this->openCL.loadKernel("estimateDensity");
    }
    this->openCL.kernel("estimateDensity")->setArg(0, this->parameterBuffer);
    this->openCL.kernel("estimateDensity")->setArg(1, this->particles);
    this->openCL.kernel("estimateDensity")->setArg(2, this->sortedParticleToCell);
    this->openCL.kernel("estimateDensity")->setArg(3, this->gridCellOffsets);
    this->openCL.kernel("estimateDensity")->setArg(4, this->numParticles);
    this->openCL.kernel("estimateDensity")->setArg(5, cellsX);
    this->openCL.kernel("estimateDensity")->setArg(6, cellsY);
    this->openCL.kernel("estimateDensity")->setArg(7, cellsZ);
    this->openCL.kernel("estimateDensity")->setArg(8, minExt);
    this->openCL.kernel("estimateDensity")->setArg(9, maxExt);
    this->openCL.kernel("estimateDensity")->setArg(10, this->density);
    
    // KERNEL :: computeLambda

    if (load) {
        this->openCL.loadKernel("computeLambda");
    }
    this->openCL.kernel("computeLambda")->setArg(0, this->parameterBuffer);
    this->openCL.kernel("computeLambda")->setArg(1, this->particles);
    this->openCL.kernel("computeLambda")->setArg(2, this->sortedParticleToCell);
    this->openCL.kernel("computeLambda")->setArg(3, this->gridCellOffsets);
    this->openCL.kernel("computeLambda")->setArg(4, this->density);
    this->openCL.kernel("computeLambda")->setArg(5, this->numParticles);
    this->openCL.kernel("computeLambda")->setArg(6, cellsX);
    this->openCL.kernel("computeLambda")->setArg(7, cellsY);
    this->openCL.kernel("computeLambda")->setArg(8, cellsZ);
    this->openCL.kernel("computeLambda")->setArg(9, minExt);
    this->openCL.kernel("computeLambda")->setArg(10, maxExt);
    this->openCL.kernel("computeLambda")->setArg(11, this->lambda);
    
    // KERNEL :: computePositionDelta

    if (load) {
        this->openCL.loadKernel("computePositionDelta");
    }
    this->openCL.kernel("computePositionDelta")->setArg(0, this->parameterBuffer);
    this->openCL.kernel("computePositionDelta")->setArg(1, this->particles);
    this->openCL.kernel("computePositionDelta")->setArg(2, this->sortedParticleToCell);
    this->openCL.kernel("computePositionDelta")->setArg(3, this->gridCellOffsets);
    this->openCL.kernel("computePositionDelta")->setArg(4, this->numParticles);
    this->openCL.kernel("computePositionDelta")->setArg(5, this->lambda);
    this->openCL.kernel("computePositionDelta")->setArg(6, cellsX);
    this->openCL.kernel("computePositionDelta")->setArg(7, cellsY);
    this->openCL.kernel("computePositionDelta")->setArg(8, cellsZ);
    this->openCL.kernel("computePositionDelta")->setArg(9, minExt);
    this->openCL.kernel("computePositionDelta")->setArg(10, maxExt);
    this->openCL.kernel("computePositionDelta")->setArg(11, this->posDelta);
    
    // KERNEL :: updatePositionDelta

    if (load) {
        this->openCL.loadKernel("updatePositionDelta");
    }
    this->openCL.kernel("updatePositionDelta")->setArg(0, this->posDelta);
    this->openCL.kernel("updatePositionDelta")->setArg(1, this->particles);

    // KERNEL :: resolveCollisions

    if (load) {
        this->openCL.loadKernel("resolveCollisions");
    }
    this->openCL.kernel("resolveCollisions")->setArg(0, this->parameterBuffer);
    this->openCL.kernel("resolveCollisions")->setArg(1, this->particles);
    this->openCL.kernel("resolveCollisions")->setArg(2, minExt);
    this->openCL.kernel("resolveCollisions")->setArg(3, maxExt);

    // KERNEL :: computeCurl

    if (load) {
        this->openCL.loadKernel("computeCurl");
    }
    this->openCL.kernel("computeCurl")->setArg(0, this->parameterBuffer);
    this->openCL.kernel("computeCurl")->setArg(1, this->particles);
    this->openCL.kernel("computeCurl")->setArg(2, this->sortedParticleToCell);
    this->openCL.kernel("computeCurl")->setArg(3, this->gridCellOffsets);
    this->openCL.kernel("computeCurl")->setArg(4, this->numParticles);
    this->openCL.kernel("computeCurl")->setArg(5, cellsX);
    this->openCL.kernel("computeCurl")->setArg(6, cellsY);
    this->openCL.kernel("computeCurl")->setArg(7, cellsZ);
    this->openCL.kernel("computeCurl")->setArg(8, minExt);
    this->openCL.kernel("computeCurl")->setArg(9, maxExt);
    this->openCL.kernel("computeCurl")->setArg(10, this->curl);
    
    // KERNEL ::  updatePosition
    
    if (load) {
        this->openCL.loadKernel("updatePosition");
    }
    this->openCL.kernel("updatePosition")->setArg(0, this->parameterBuffer);
    this->openCL.kernel("updatePosition")->setArg(1, this->dt);
    this->openCL.kernel("updatePosition")->setArg(2, this->particles);
    this->openCL.kernel("updatePosition")->setArg(3, this->sortedParticleToCell);
    this->openCL.kernel("updatePosition")->setArg(4, this->gridCellOffsets);
    this->openCL.kernel("updatePosition")->setArg(5, this->numParticles);
    this->openCL.kernel("updatePosition")->setArg(6, this->curl);
    this->openCL.kernel("updatePosition")->setArg(7, cellsX);
    this->openCL.kernel("updatePosition")->setArg(8, cellsY);
    this->openCL.kernel("updatePosition")->setArg(9, cellsZ);
    this->openCL.kernel("updatePosition")->setArg(10, minExt);
    this->openCL.kernel("updatePosition")->setArg(11, maxExt);
    this->openCL.kernel("updatePosition")->setArg(12, this->renderPos);
    
    // Set up the kernels for computing a prefix sum ("scan") in parallel:
    this->prefixSum = shared_ptr<PrefixSum>(new PrefixSum(this->openCL));
}

/******************************************************************************/

/**
 * Resets the current simulation stats bounding box back to the initial
 * dimensions the were in place at the beginning of the simulation
 */
void Simulation::resetBounds()
{
    this->animFrameNumber = 0;
    this->bounds = this->originalBounds;
}

/**
 * Resets the state of the simulation
 */
void Simulation::reset()
{
    this->frameNumber = 0;
    this->initializeBuffers();
    this->setupKernels(false);
    this->resetBounds();
    this->writeToGPU();
}

/**
 * Steps the simulation's bounding box animation, if enabled, by one frame
 */
void Simulation::stepBoundsAnimation()
{
    float value     = 0.0f;
    float pi        = static_cast<float>(M_PI);
    float t         = static_cast<float>(this->animFrameNumber);
    float origMinX  = this->originalBounds.getMinExtent().x;
    float origMaxX  = this->originalBounds.getMaxExtent().x;
    float width     = origMaxX - origMinX;
    float limit     = width * 0.66;
    float halfLimit = limit * 0.5f;
    float limitMinX = origMinX + halfLimit;
    float limitMaxX = origMaxX - halfLimit;
    
    if (this->animType == SINE_WAVE) {
        
        float theta = ofDegToRad(static_cast<float>(this->animFrameNumber % 720));
        value   = this->animAmp * sin(this->animPeriod * pi * theta);

        this->bounds.getMaxExtent().x = origMaxX - value;
        
        if (this->animBothSides) {
            this->bounds.getMinExtent().x = origMinX + value;
        }
        
    } else if (this->animType == LINEAR_RAMP) {
        
        float tPeriod = (t / this->animPeriod) * this->dt;
        value = 2.0f * this->animAmp * (tPeriod - floor(0.5f + tPeriod));
    
        this->bounds.getMaxExtent().x = origMaxX - value;
        
        if (this->animBothSides) {
            this->bounds.getMinExtent().x = origMinX + value;
        }
        
    } else if (this->animType == COMPRESS) {

        if (this->bounds.getMaxExtent().x >= limitMaxX) {
            this->bounds.getMaxExtent().x -= 0.25f;
        }

        if (this->animBothSides && this->bounds.getMinExtent().x <= limitMinX) {
            this->bounds.getMinExtent().x += 0.25f;
        }
    }
    
    this->animFrameNumber++;
}

/**
 * Moves the state of the simulation forward one time step according to the
 * time step value, dt, passed to the constructor
 *
 * In this method, the motion of the particles, as well as the various
 * quatities assigned to them are updated, as described in the paper
 * "Position Based Fluids" by Miles Macklin & Matthias Muller.
 */
void Simulation::step()
{
    // Solver iterations (this will be adjustable later)

    int N = Constants::SOLVER_ITERATIONS;

    // Intialize the simulation step:
    
    this->resetQuantities();
    
    // Where the actual work is done: the sequence of substeps follows
    // more-or-less from the listing "Algorithm 1 Simulation Loop" in the
    // paper "Position Based Fluids". The main difference is that we are using
    // a different method than Macklin and Muller to compute the nearest
    // neighbors of a given particle. Whereas they use the method by
    // [Green 2008], we use the method described by Hoetzlein, 2014
    // in the slides
    // "￼FAST FIXED-RADIUS NEAREST NEIGHBORS: INTERACTIVE MILLION-PARTICLE FLUID"
    // that uses counting sort as an alternative to radix sort

    this->predictPositions(); // See (1) - (4)
    
    this->findNeighboringParticles(); // See (5) - (7)

    // Solver runs for N iterations:

    for (int i = 0; i < N; i++) { // See (8) - (19)

        this->calculateDensity(); // See (9) - (12)

        this->calculatePositionDelta(); // See (13)

        //this->handleCollisions(); // See (14)
        
        this->updatePositionDelta(); // See (17)
    }

    this->updatePosition(); // See (20) - (24)
    
    // Make sure the OpenCL work queue is empty before proceeding. This will
    // block until all the stuff in GPU-land is done before moving forward
    // and reading the results of the work we did on the GPU back into
    //host-land:

    this->openCL.finish();
    
    // Read the changes back from the GPU so we can manipulate the values
    // in our C++ program:

#ifdef DRAW_PARTICLES_AS_SPHERES

    this->readFromGPU();

#else

    // If rendering particles using GL_POINTS, we don't need to read anything
    // back from the GPU

#endif

    // Animate the bounds of the simulation to generate waves in the particles:

    if (this->animBounds) {
        this->stepBoundsAnimation();
    }
    
    // Finally, bump up the frame counter:

    this->frameNumber++;
}

/******************************************************************************/

/**
 * Draws the cell grid
 *
 * @param [in] The current world position of the camera cameraPosition
 */
void Simulation::drawGrid(const ofCamera& camera)
{
    auto p1 = this->bounds.getMinExtent();
    auto p2 = this->bounds.getMaxExtent();
    
    float xCellWidth = (p2.x - p1.x) / static_cast<float>(this->cellsPerAxis.x);
    float halfXWidth = xCellWidth * 0.5f;
    float yCellWidth = (p2.y - p1.y) / static_cast<float>(this->cellsPerAxis.y);
    float halfYWidth = yCellWidth * 0.5f;
    float zCellWidth = (p2.z - p1.z) / static_cast<float>(this->cellsPerAxis.z);
    float halfZWidth = zCellWidth * 0.5f;
    
    ofNoFill();
    ofSetColor(0, 255, 0);

    for (int i = 1; i < (2  * this->cellsPerAxis.x); i += 2) {

        float xCorner = p1.x + (static_cast<float>(i) * halfXWidth);

        for (int j = 1; j < (2  * this->cellsPerAxis.y); j += 2) {
        
            float yCorner = p1.y + (static_cast<float>(j) * halfYWidth);
        
            for (int k = 1; k < (2  * this->cellsPerAxis.z); k += 2) {
                
                float zCorner = p1.z + (static_cast<float>(k) * halfZWidth);

                ofDrawBox(xCorner, yCorner, zCorner, xCellWidth, yCellWidth, zCellWidth);
            }
        }
    }
}

/**
 * Draws the bounds of the simulated environment as a transparent with solid
 * lines indicating the edges of the bounding box.
 *
 * @param [in] The current scene camera
 */
void Simulation::drawBounds(const ofCamera& camera)
{
    // Draw the bounding box that will hold the particles:
    auto p1 = this->bounds.getMinExtent();
    auto p2 = this->bounds.getMaxExtent();

    auto x  = (p1.x + p2.x) * 0.5f;
    auto y  = (p1.y + p2.y) * 0.5f;
    auto z  = (p1.z + p2.z) * 0.5f;
    auto w  = p2.x - p1.x;
    auto h  = p2.y - p1.y;
    auto d  = p2.z - p1.z;
    
    ofNoFill();
    ofSetColor(255, 255, 255);
    ofDrawBox(x, y, z, w, h, d);
}

/**
 * Currently, draws the positions of the particles using a fixed color. 
 * Later, this may be changed so that the color of the particle reflects
 * some quantity like velocity, mass, viscosity, etc.
 *
 * @param [in] The current scene camera
 */
void Simulation::drawParticles(const ofCamera& camera)
{
    auto cp              = camera.getPosition();
    float particleRadius = this->getParameters().particleRadius ;
    
#if DRAW_PARTICLES_AS_SPHERES

    this->shader.begin();
        this->shader.setUniform3f("cameraPosition", cp.x, cp.y, cp.z);
        for (int i = 0; i < this->numParticles; i++) {
            Particle &p = this->particles[i];
            ofPushMatrix();
                ofTranslate(p.pos.x, p.pos.y, p.pos.z);
                this->particleMesh.draw();
            ofPopMatrix();
        }
    this->shader.end();

#else
    
    this->shader.begin();
        this->shader.setUniform1f("particleRadius", particleRadius * 50.0f);
        this->shader.setUniform3f("cameraPosition", cp.x, cp.y, cp.z);
        this->particleVertices.draw(GL_POINTS, 0, this->numParticles);
    this->shader.end();

#endif

    // Use visual debugging? If so, we can see the IDs assigned to individual
    // particle

    if (this->isVisualDebuggingEnabled()) {
        for (int i = 0; i < this->numParticles; i++) {
            Particle &p = this->particles[i];
            // Label the particle with its number:
            ofSetColor(255, 255, 0);
            ofFill();
            ofPushMatrix();
                ofTranslate(0,0,p.pos.z);
                ofDrawBitmapString(ofToString(i), p.pos.x, p.pos.y);
            ofPopMatrix();
        }
    }
}

/**
 * This method is once per step of the simulation to render all graphical
 * output, including rendering the bounding box of the simulated environment,
 * all particles in the simulation, as well as any additional objects (meshs,
 * walls, etc.) that may exist.
 *
 * @param [in] The current scene camera
 */
void Simulation::draw(const ofCamera& camera)
{
    this->drawBounds(camera);

    if (this->drawGridEnabled()) {
        this->drawGrid(camera);
    }

    this->drawParticles(camera);

    ofDrawAxis(2.0f);
}

/******************************************************************************/

/**
 * This implementation is based off of the method described in
 * "￼FAST FIXED-RADIUS NEAREST NEIGHBORS: INTERACTIVE MILLION-PARTICLE FLUID" by
 * Hoetzlein, 2014 that uses counting sort as an alternative to radix sort.
 *
 * See http://on-demand.gputechconf.com/gtc/2014/presentations/S4117-fast-fixed-radius-nearest-neighbor-gpu.pdf
 */
void Simulation::findNeighboringParticles()
{
    this->discretizeParticlePositions();
    this->sortParticlesByCell();
}

/**
 * Resets various particle quantities, like density, etc.
 *
 * @see kernels/Simulation.cl (resetParticleQuantities, resetCellQuantities) for details
 */
void Simulation::resetQuantities()
{
    this->openCL.kernel("resetParticleQuantities")->run1D(this->numParticles);
    this->openCL.kernel("resetCellQuantities")->run1D(this->numCells);
}

/**
 * Updates the predicted positions of the particles via an explicit Euler step
 *
 * @see kernels/Simulation.cl (predictPosition) for details
 */
void Simulation::predictPositions()
{
    this->openCL.kernel("predictPosition")->run1D(this->numParticles);
}

/**
 * Discretizes all of the particles to a grid cell, where the number of
 * grid cells along each axis in the simulated space is specified by 
 * cellsPerAxis, e.g. (4,5,6) specifies 4 cells in the x-axs, 5 in the y-axis, 
 * and 6 in the z-axis
 *
 * @see kernels/Simulation.cl (discretizeParticlePositions) for details
 */
void Simulation::discretizeParticlePositions()
{
    this->openCL.kernel("discretizeParticlePositions")->setArg(6, this->bounds.getMinExtent());
    this->openCL.kernel("discretizeParticlePositions")->setArg(7, this->bounds.getMaxExtent());
    this->openCL.kernel("discretizeParticlePositions")->run1D(this->numParticles);
}

/**
 * Sorts the particles by the assigned grid cell. Following the run of this
 * function, sortedParticleToCell (on the GPU) will contain a
 * listing of ParticlePosition, that are sorted by linearized cell indices, e.g.
 * particles that are in the same cell will be consecutive in 
 * sortedParticleToCell, making neighbor search quick.
 *
 * @see kernels/Simulation.cl (countSortParticles) for details
 */
void Simulation::sortParticlesByCell()
{
    // First, compute the prefix sums of the entries of the cell histogram:
    
    this->prefixSum->scan(this->cellPrefixSums, this->cellHistogram, this->numCells);
    
    // next, use the prefix sums to determine the sorted position of the
    // particles:
    
    this->openCL.kernel("countSortParticlesByCell")->run1D(this->numParticles);

    //this->openCL.kernel("debugHistogram")->run1D(1);
    //this->openCL.kernel("debugSorting")->run1D(1);
    
    // Finally, compute the bins/cells that each particle is in and store
    // the (start,length) tuples for each grid cell span in an array, which we
    // will use for lookup later:
    
    this->openCL.kernel("findParticleBins")->run1D(this->numParticles);
}

/**
 * Computes the density for each particle using the SPH density estimator
 * 
 * (*) Specifically, this function is part of the constraint solver loop
 *
 * @see kernels/Simulation.cl (estimateDensity) for details
 */
void Simulation::calculateDensity()
{
    this->openCL.kernel("estimateDensity")->setArg(8, this->bounds.getMinExtent());
    this->openCL.kernel("estimateDensity")->setArg(9, this->bounds.getMaxExtent());
    this->openCL.kernel("estimateDensity")->run1D(this->numParticles);
}

/**
 * Computes the position delta
 *
 * (*) Specifically, this function is part of the constraint solver loop
 *
 * @see kernels/Simulation.cl (computeLambda, computePositionDelta) for details
 */
void Simulation::calculatePositionDelta()
{
    this->openCL.kernel("computeLambda")->setArg(9, this->bounds.getMinExtent());
    this->openCL.kernel("computeLambda")->setArg(10, this->bounds.getMaxExtent());
    this->openCL.kernel("computeLambda")->run1D(this->numParticles);

    this->openCL.kernel("computePositionDelta")->setArg(9, this->bounds.getMinExtent());
    this->openCL.kernel("computePositionDelta")->setArg(10, this->bounds.getMaxExtent());
    this->openCL.kernel("computePositionDelta")->run1D(this->numParticles);
}

/**
 * Apply the position delta
 *
 * @see kernels/Simulation.cl (updatePositionDelta) for details
 */
void Simulation::updatePositionDelta()
{
    this->openCL.kernel("updatePositionDelta")->run1D(this->numParticles);
}

/**
 * TODO: All this does now is clamp the particle positions to the simulation
 * bounding box
 *
 * @see kernels/Simulation.cl (resolveCollisions) for details
 */
void Simulation::handleCollisions()
{
    this->openCL.kernel("resolveCollisions")->setArg(2, this->bounds.getMinExtent());
    this->openCL.kernel("resolveCollisions")->setArg(3, this->bounds.getMaxExtent());
    this->openCL.kernel("resolveCollisions")->run1D(this->numParticles);
}

/**
 * Updates the actual, final position of the particles in the current
 * simulation step
 *
 * @see kernels/Simulation.cl (computeCurl, updatePosition) for details
 */
void Simulation::updatePosition()
{
    this->openCL.kernel("computeCurl")->setArg(8, this->bounds.getMinExtent());
    this->openCL.kernel("computeCurl")->setArg(9, this->bounds.getMaxExtent());
    this->openCL.kernel("computeCurl")->run1D(this->numParticles);

    this->openCL.kernel("updatePosition")->setArg(10, this->bounds.getMinExtent());
    this->openCL.kernel("updatePosition")->setArg(11, this->bounds.getMaxExtent());
    this->openCL.kernel("updatePosition")->run1D(this->numParticles);
}

/******************************************************************************/
