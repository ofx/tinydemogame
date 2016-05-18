#include <windows.h>
#include <math.h>

#define GLEW_STATIC
#include <GL/glew.h>
#include <GL/gl.h>
#include <GL/glu.h>

#include <chipmunk.h>

#include "shaders.h"

// Constants
#define MAX_VEHICLE_WHEELS		4		// Array boundary
#define MAX_VEHICLES			100		// Array boundary
#define MAX_ROCKS				100		// Array boundary

#define COUNT_WHEELS_CAR		2		// #wheels for a car
#define COUNT_WHEELS_TRAILER	3		// #wheels for a trailer

#define	TERRAIN_WIDTH			50		// Terrain width in world coordinates
#define	TERRAIN_SEGMENTS		200		// Terrain width in number of (physics) segments
#define	WORLD_SCALE				0.14f	// Scale of objects in world coordinates

#define PLAYER_W_LIMIT			1.8f				// Player character rotational limit
#define CAMERA_FOLLOW_MIN		-(WORLD_SCALE * 1)	// Margin between the player character and the camera
#define CAMERA_FOLLOW_MAX		(WORLD_SCALE * 1)

// Types and structs
struct VertexData
{
	float x, y, z;
	float nx, ny, nz;
	float s, t;
};

struct HeightData
{
	cpFloat y, a;
};

struct WheelData
{
	cpShape*		wheel;
	cpConstraint*	spring;
	cpConstraint*	joint;
	bool			attached;
};

struct VehicleData
{
	cpShape*		chassis;
	cpConstraint*	link;
	WheelData		wheel[MAX_VEHICLE_WHEELS];

	unsigned int	carType : 4;
	unsigned int	npc : 1;
};

struct RockData
{
	cpShape*		rock;
};

struct CameraData
{
	cpShape*		pivot;
	cpShape*		player;
};

// World to be used for physics
cpSpace* space;
cpBody*  bounds;

bool bRun = true;

CameraData		g_Camera;

// Global physics objects
unsigned int	g_nNpcVehicles;
unsigned int	g_nPcVehicles;
unsigned int	g_nVehicles;
unsigned int	g_nRocks;
VehicleData*	g_pVehicles;
RockData*		g_pRocks;

// Misc. Variables
float			g_fBoost;
float			g_fTerrainStep;

// Physics groups
#define	GROUP_DEFAULT	0
#define	GROUP_PC		1		// Player character vehicles
#define	GROUP_NPC		100		// Non-player character vehicles

// Handling types
#define	HANDLING_ACCELERATE	0
#define	HANDLING_BRAKE		1
#define	HANDLING_BOOST		2

// Height map data
HeightData* g_pRoadHeightMap;
HeightData* g_pMountainHeightMap;

// Scroll position
float g_fXScroll;

// Window handlers
HDC			hDC;
HGLRC		hRC;
HWND		hWnd;
HINSTANCE	hInstance;

// Bools indicating keys pressed, active window and fullscreen
bool g_bKeys[256];
bool g_bKeyLock;
bool g_bActiveWindow;

// OpenGL declarations
GLuint textures[3];
GLuint buffers[4];
GLuint programLighting, programEdge;

GLuint fontList;

GLuint textureDepth;
GLuint textureColor;
GLuint textureNormal;
GLuint fbo;

GLubyte data[3][256][256][4];

// Forward declaration of WndProc
LRESULT	CALLBACK WndProc( HWND, UINT, WPARAM, LPARAM );

// Define pointers to glExtSwapIntervalProc (vsync)
typedef void (APIENTRY *PFNWGLEXTSWAPCONTROLPROC) (int);
typedef int  (*PFNWGLEXTGETSWAPINTERVALPROC)      (void);
 
PFNWGLEXTSWAPCONTROLPROC     wglSwapIntervalEXT	   = NULL;
PFNWGLEXTGETSWAPINTERVALPROC wglGetSwapIntervalEXT = NULL;

// Score count
int				g_nScore;
float			g_fMultiplier;
DWORD			g_dwLastScoreTime;
unsigned int	g_nLevel;

// Stride macro
#define BUFFER_OFFSET( i ) ((GLvoid*) i)

#define M_PI 3.14159265f

// Physics collision types
#define T_ROCK			  1	 // Rock after bounce on heightmap
#define T_WHEEL_TRAILER   2  // Wheel (trailer)
#define T_MOUNTAIN		  3	 // Mountain (heightmap)
#define T_ROAD			  4	 // Road (heightmap)
#define T_WHEEL			  5	 // Wheel (car)
#define T_CHASSIS		  6	 // Chassis
#define T_LEFT_BOUNDARY	  7	 // Left level boundary
#define T_RIGHT_BOUNDARY  8	 // Right level boundary
#define T_FINISH		  9  // Level finish line
#define T_BOTTOM_BOUNDARY 10 // Bottom boundary

// Physics layers (bitplanes)
#define LAYER_DEFAULT	1
#define LAYER_BOTTOM	2

// Robert Jenkins' 32 bit integer hash function
// From: http://www.cris.com/~Ttwang/tech/inthash.htm
inline unsigned int random( unsigned int a )
{
    a = (a+0x7ed55d16) + (a<<12);
    a = (a^0xc761c23c) ^ (a>>19);
    a = (a+0x165667b1) + (a<<5);
    a = (a+0xd3a2646c) ^ (a<<9);
    a = (a+0xfd7046c5) + (a<<3);
    a = (a^0xb55a4f09) ^ (a>>16);

    return a;
}

// rand() replacement using the above hash function
unsigned int g_nSeed;
void _srand( unsigned int seed )
{
	g_nSeed = seed;
}
unsigned int _rand( void )
{
	return random( g_nSeed++ );
}

///***********************************************************///
/// Update physics routines
///***********************************************************///

// Post step callback, for safe removal of bodies
static void RemoveBody( cpSpace *space, cpShape *shape, void *data )
{
	cpSpaceRemoveShape( space, shape );
	cpShapeFree( shape );
}

// Post step callback, for safe addition of bodies
static void AddBody( cpSpace *space, cpShape *shape, void *data )
{
    cpSpaceAddBody(space, shape->body);
        
    cpSpaceAddShape(space, shape);
}

void UpdateScore()
{
	int currentTime = GetTickCount();
	int scoreTimeDifference = currentTime  - g_dwLastScoreTime;

	if( scoreTimeDifference <= 300 )
	{
		g_fMultiplier += 0.2f;
	}
	else
	{
		g_fMultiplier = 1.0f;
	}

	g_nScore += (10.0f * int( g_fMultiplier ));

	g_dwLastScoreTime = currentTime;
}

// Apply impulse to non-player character, call every frame to let npc's move
void NpcApplyImpulse()
{
	for( int i = 0 ; i < g_nVehicles; ++i )
	{
		if( g_pVehicles[i].npc )
		{
			cpBody* pBody = g_pVehicles[i].chassis->body;
			cpBodyApplyImpulse( pBody, cpv( -WORLD_SCALE * 0.25f + fmod( pBody->a, M_PI / 2.0 ) / M_PI * 0.15f, 0.0f ), cpvzero );
		}
	}
}

void HandlePcVehicle( const unsigned int handlingType )
{
	cpBody* pPcCar = g_pVehicles[0].chassis->body;
	
	if( handlingType == HANDLING_ACCELERATE )
	{	// Accelerate
		if( pPcCar->v.x < WORLD_SCALE * 25.0f )
		{
			cpBodyApplyImpulse( pPcCar, cpv( (WORLD_SCALE * 0.20f + pPcCar->a / M_PI * 0.15f) * (g_nPcVehicles * 1.50f), 0.0f ), cpvzero );
		}
	}
	else if( handlingType == HANDLING_BRAKE )
	{	// Brake
		if( pPcCar->v.x > 0 )
		{
			cpBodyApplyImpulse( pPcCar, cpv( -WORLD_SCALE * 0.5, 0.0f ), cpvzero );
		}
	}
	else if( handlingType == HANDLING_BOOST )
	{	// Rocket boost
		g_fBoost = 0.01f;
	}
}

// Kills a rock
static void KillRock( RockData * pRock )
{
	pRock->rock->layers = LAYER_BOTTOM;
}

// Kills a vehicle
static void KillVehicle( VehicleData * pVehicleData )
{
	int i;

	pVehicleData->chassis->layers = LAYER_BOTTOM;

	for( i = 0; i < MAX_VEHICLE_WHEELS; ++i )
	{
		if( pVehicleData->wheel[i].wheel )
			pVehicleData->wheel[i].wheel->layers = LAYER_BOTTOM;
	}
}

static int KillNpcHandler( cpArbiter* pArbiter, struct cpSpace* pSpace, void* pData )
{
	int i;

	CP_ARBITER_GET_SHAPES( pArbiter, a, b );

	if( a->collision_type == T_WHEEL || a->collision_type == T_CHASSIS )
	{
		// Collided with a vehicle
		VehicleData * pVehicleData = (VehicleData *)a->data;
		if( pVehicleData->npc )
			KillVehicle( pVehicleData );
	}
	else if( a->collision_type == T_ROCK )
	{
		// Collided with a rock
		RockData * pRockData = (RockData *)a->data;
		KillRock( pRockData );
	}

	if( b->collision_type == T_WHEEL || b->collision_type == T_WHEEL_TRAILER )
	{
		// Colliding wheel
		VehicleData * pVehicleData = (VehicleData *)b->data;
		for( i = 0; i < MAX_VEHICLE_WHEELS; ++i )
		{
			// Find this wheel in the vehicle data
			if( pVehicleData->wheel[i].wheel == b )
			{
				// Kill it
				pVehicleData->wheel[i].attached = false;
				b->layers = LAYER_BOTTOM;
			}
		}
	}

	UpdateScore();

	return TRUE;
}

void ApplyBoost( void )
{
	float impulse = 1 / 1 + exp( -g_fBoost ) * WORLD_SCALE;
	int i;

	cpVect vec = cpv( impulse * impulse * WORLD_SCALE * 3.0f, WORLD_SCALE * 2.5f );

	for( i = 0; i < 1; ++i )
	{
		if( !g_pVehicles[i].npc )
		{
			cpBodyApplyImpulse( g_pVehicles[i].chassis->body, vec, cpvzero );

			if( g_fBoost < 1 )
			{
				g_pVehicles[i].chassis->body->w_limit = 0.1f;
			}
			else
			{
				g_pVehicles[i].chassis->body->w_limit = PLAYER_W_LIMIT;
			}
		}
	}

	if( g_fBoost < 1 )
	{
		g_fBoost += 0.1f;
	}
	else
	{
		g_fBoost = 0;
	}
}

// Detached a wheel from a vehicle
bool DetachWheel( VehicleData * pVehicleData, WheelData ** pWheelDetached )
{
	int i = 0;
	*pWheelDetached = NULL;

	// Find the first existing wheel
	while( pVehicleData->wheel[i].wheel == NULL || !pVehicleData->wheel[i].attached ) { ++i; }
	
	// Check if there are any wheels left
	if( i == MAX_VEHICLE_WHEELS ) return false;
	*pWheelDetached = &pVehicleData->wheel[i];

	// Remove the constraints
	(*pWheelDetached)->attached = false;
	cpSpaceRemoveConstraint( space, (*pWheelDetached)->spring );
	cpSpaceRemoveConstraint( space, (*pWheelDetached)->joint );

	return true;
}

void ShootAxle( void )
{
	int i;
	cpVect launchVector = cpv( WORLD_SCALE * 4.0f, 0.0f );
	bool f = false;

	// Find a trailer with wheels and detach a wheel
	for( i = (g_nVehicles - 1); i > 0; --i )
	{
		if( !g_pVehicles[i].npc )
		{
			WheelData * pWheel = NULL;
			if( DetachWheel( &g_pVehicles[i], &pWheel ) )
			{
				f = true;

				// Wheel was detached, so launch it
				cpBodyApplyImpulse( pWheel->wheel->body, launchVector, cpvzero );
				break;
			}
		}
	}

	if( !f )
	{
		bRun = false;
	}
}

// Update physics space and spawn a rock at a certain time interval
void UpdateSpace( void )
{
	static int lastRockSpawn;
	int currentTime = GetTickCount();

	const cpFloat physicsRate = 60.0f;

	int steps = 5;
	cpFloat dt = 1.0f / physicsRate / (cpFloat) steps;

	for( int i = 0 ; i < steps ; ++i ){
		cpSpaceStep( space, dt );
	}

	NpcApplyImpulse();

	if( g_fBoost > 0.0f )
	{
		ApplyBoost();
	}
}

///***********************************************************///

///***********************************************************///
/// Draw functions
///***********************************************************///

// Draw a cuboid
GLvoid DrawCuboid( const cpVect & a, const cpVect & b, const cpVect & c, const cpVect & d, float z ) {
	glBegin( GL_QUADS );
		glNormal3d( 0, 0, -1 );
		glVertex3d( a.x, a.y, -z );
		glVertex3d( b.x, b.y, -z );
		glVertex3d( c.x, c.y, -z );
		glVertex3d( d.x, d.y, -z );

		glNormal3d( 0, 0, 1 );
		glVertex3d( a.x, a.y,  z );
		glVertex3d( b.x, b.y,  z );
		glVertex3d( c.x, c.y,  z );
		glVertex3d( d.x, d.y,  z );

		glNormal3d( 0, -1, 0 );
		glVertex3d( b.x, b.y, -z );
		glVertex3d( b.x, b.y,  z );
		glVertex3d( a.x, a.y,  z );
		glVertex3d( a.x, a.y, -z );

		glNormal3d( 0, 1, 0 );
		glVertex3d( c.x, c.y, -z );
		glVertex3d( c.x, c.y,  z );
		glVertex3d( d.x, d.y,  z );
		glVertex3d( d.x, d.y, -z );

		glNormal3d( -1, 0, 0 );
		glVertex3d( b.x, b.y, -z );
		glVertex3d( b.x, b.y,  z );
		glVertex3d( c.x, c.y,  z );
		glVertex3d( c.x, c.y, -z );

		glNormal3d( 1, 0, 0 );
		glVertex3d( a.x, a.y, -z );
		glVertex3d( a.x, a.y,  z );
		glVertex3d( d.x, d.y,  z );
		glVertex3d( d.x, d.y, -z );
	glEnd();
}

char vehicleData[] = {
	2,								// #types
	
	5,	2,
		-2, -2,
		-2,  1,
		 1,  1,
		 2,  0,
		 2, -2,

	4,	1,
		-1, -1,
		-1,  1,
		 1,  1,
		 1, -1
};

char* const GetVehicleData( unsigned int nType, unsigned int & nVertices, unsigned int & nScale )
{
	unsigned int i, j = 0;
	char * ptr = NULL;
	nVertices = 0;

	if( nType < vehicleData[j++] )
	{
		for( i = 0; i <= nType; ++i )
		{
			const char n = vehicleData[j++];
			nScale = vehicleData[j++];
			nVertices = n;
			ptr = &vehicleData[j];
			j += n * 2;
		}
	}
	return ptr;
}

GLvoid DrawShape( unsigned int nType, float fScale, float z )
{
	unsigned int i, j;
	unsigned int nVertices, nScale;
	char * pVertices = GetVehicleData( nType, nVertices, nScale );

	// Front
	glBegin( GL_TRIANGLE_FAN );
		glNormal3f( 0, 0, -1 );
		for( j = 0; j <= (nVertices + 1); ++j )
		{
			i = j % nVertices;
			glVertex3f( float( pVertices[i*2] ) / float( nScale ) * fScale, float( pVertices[i*2+1] ) / float( nScale ) * fScale, -z );
		}
	glEnd();
	// Back
	glBegin( GL_TRIANGLE_FAN );
		glNormal3f( 0, 0, 1 );
		for( j = 0; j <= (nVertices + 1); ++j )
		{
			i = j % nVertices;
			glVertex3f( float( pVertices[i*2] ) / float( nScale ) * fScale, float( pVertices[i*2+1] ) / float( nScale ) * fScale, z );
		}
	glEnd();
	// Sides
	glBegin( GL_QUAD_STRIP );
		for( j = 0; j <= (nVertices + 1); ++j )
		{
			i = j % nVertices;
			GLfloat x = float( pVertices[i*2] ) / float( nScale ) * fScale;
			GLfloat y = float( pVertices[i*2+1] ) / float( nScale ) * fScale;
			glNormal3f( x, y, 0 );	// TODO: fix normals
			glVertex3f( x, y, -z );
			glVertex3f( x, y, z );
		}
	glEnd();
}

// Draw a wheel
GLvoid DrawWheel( unsigned int n, float z ) {
	unsigned int i;

	const unsigned int nMax = 31;
	if( n >= nMax ) n = nMax;
	GLfloat x[nMax+1], y[nMax+1];

	for( i = 0 ; i <= n; ++i ) {
		GLfloat t = M_PI * 2 * (-0.5f + (GLfloat)(int( i ) - 1) / n);
		x[i] = sin( t );
		y[i] = cos( t );
	}

	// Front
	glBegin( GL_TRIANGLE_FAN );
		glNormal3f( 0, 0, -1 );
		for( i = 0; i <= n; ++i ) glVertex3f( x[i], y[i], -z );
	glEnd();
	// Back
	glBegin( GL_TRIANGLE_FAN );
		glNormal3f( 0, 0, 1 );
		for( i = 0; i <= n; ++i ) glVertex3f( x[i], y[i], z );
	glEnd();
	// Sides
	glBegin( GL_QUAD_STRIP );
		for( i = 0; i <= n; ++i ) {
			glNormal3f( x[i], y[i], 0 );
			glVertex3f( x[i], y[i], -z );
			glVertex3f( x[i], y[i], z );
		}
	glEnd();
}

// Draw a rock (sphere with lats and longs and displacement)
GLvoid DrawRock( GLint nLats, GLint nLongs, GLfloat fDisplacement ) {
	int i, j;

	for( i = 0 ; i <= nLats ; ++i ) {
		#define IS_EDGE( x ) ((x == 0) || (x == nLats))

		GLdouble lat0 = M_PI * (-0.5 + (GLdouble) (i - 1) / nLats);
		GLdouble z0   = sin(lat0);
		GLdouble zr0  =  cos(lat0);
    
		GLdouble lat1 = M_PI * (-0.5 + (GLdouble) i / nLats);
		GLdouble z1   = sin(lat1);
		GLdouble zr1  = cos(lat1);
    
		glBegin( GL_QUAD_STRIP );
			for( j = 0 ; j <= nLongs ; ++j ) {
				GLdouble lng = 2 * M_PI * (GLdouble) (j - 1) / nLongs;
				GLdouble x   = cos(lng);
				GLdouble y   = sin(lng);
				
				// Add displacement whenever point is not on edge
				const GLdouble displacementFactor = 1.3f;
				GLdouble d0 = (IS_EDGE( i-1 ) ? 0 : fDisplacement * (random( j << 8 | (i-1) ) % 10000 ) / 10000.0f * displacementFactor);
				GLdouble d1 = (IS_EDGE( i ) ? 0 : fDisplacement * (random( j << 8 | (i) ) % 10000 ) / 10000.0f * displacementFactor);

				GLdouble zrr0 = zr0 + d0;
				GLdouble zrr1 = zr1 + d1;
    
				glNormal3d( x * zrr0, y * zrr0, z0 );
				glVertex3d( x * zrr0, y * zrr0, z0 );
				glNormal3d( x * zrr1, y * zrr1, z1 );
				glVertex3d( x * zrr1, y * zrr1, z1 );
			}
		glEnd();
    }
}

// Draw all active shaped in the physics space -> rocks and character
void DrawActiveShapes( void * shape, void * data )
{
	const cpShape * pShape = (cpShape *)shape;
	const cpBody * pBody  = pShape->body;

	switch( pShape->collision_type )
	{
		case T_ROCK:
			{	// Rock object
				cpCircleShape * pCircle = (cpCircleShape *)pShape;
				cpVect c = cpvadd( pBody->p, cpvrotate( pCircle->c, pBody->rot ) );

				glPushMatrix();
					glBindTexture( GL_TEXTURE_2D, textures[0] );
					glTranslatef( c.x, c.y, -0.5f - (pCircle->r / 2.0f) );
					glRotatef( pBody->a * 180.0f / (cpFloat) M_PI, 0.0f, 0.0f, 1.0f );
					glScalef( pCircle->r * (WORLD_SCALE * 5.5f), pCircle->r * (WORLD_SCALE * 5.5f), pCircle->r * (WORLD_SCALE * 5.5f) );
					DrawRock( 30, 30, 0.2f );
				glPopMatrix();
			}
			break;
		case T_WHEEL_TRAILER:
		case T_WHEEL:
			{	// Wheel object
				cpCircleShape * pCircle = (cpCircleShape *)pShape;
				cpVect c = cpvadd( pBody->p, cpvrotate( pCircle->c, pBody->rot ) );

				const unsigned int nSegments = 7;
				glPushMatrix();
					glBindTexture( GL_TEXTURE_2D, textures[0] );
					glPushMatrix();
						glTranslatef( c.x, c.y, -0.5f - WORLD_SCALE );
						glRotatef( pBody->a * 180.0f / (cpFloat) M_PI, 0.0f, 0.0f, 1.0f );
						glScalef( pCircle->r, pCircle->r, pCircle->r );
						DrawWheel( nSegments, WORLD_SCALE );
					glPopMatrix();
					glPushMatrix();
						glTranslatef( c.x, c.y, -0.5f + WORLD_SCALE );
						glRotatef( pBody->a * 180.0f / (cpFloat) M_PI, 0.0f, 0.0f, 1.0f );
						glScalef( pCircle->r, pCircle->r, pCircle->r );
						DrawWheel( nSegments, WORLD_SCALE );
					glPopMatrix();
				glPopMatrix();

			}
			break;
		case T_CHASSIS:
			{
				VehicleData * pData = (VehicleData *)pShape->data;
				cpPolyShape * pPoly = (cpPolyShape*)pShape;
				cpVect * pVertices = pPoly->verts;
				const cpVect p = pBody->p;

				glPushMatrix();
					glBindTexture( GL_TEXTURE_2D, textures[0] );
					glTranslatef( p.x, p.y, -0.5f );
					glRotatef( pBody->a * 180.0f / (cpFloat) M_PI, 0.0f, 0.0f, 1.0f );
					
					if( pData->npc )
					{
						glRotatef( 180.0f, 0.0f, 1.0f, 0.0f );
					}

					DrawShape( pData->carType, WORLD_SCALE, WORLD_SCALE );
				glPopMatrix();
			}
			break;
		default:
			break;
	}
}

// Draw a single cloud with position x, y
GLvoid DrawCloud( float x, float y )
{
	glEnable( GL_BLEND );
	glEnable( GL_ALPHA_TEST );
	glAlphaFunc( GL_GREATER, 0.1f );

	glBindTexture( GL_TEXTURE_2D, textures[2] );

	glPushMatrix();
		glTranslatef( x, y, -2.5f );

		glBegin( GL_QUADS );
			glNormal3d( 0, 0, 1 );
			
			glTexCoord2f( 0.0f, 0.0f );
			glVertex3f( 0.0f, 0.0f, 0.0f );
			glTexCoord2f( 1.0f, 0.0f );
			glVertex3f( 1.0f, 0.0f, 0.0f );
			glTexCoord2f( 1.0f, 1.0f );
			glVertex3f( 1.0f, 1.0f, 0.0f );
			glTexCoord2f( 0.0f, 1.0f );
			glVertex3f( 0.0f, 1.0f, 0.0f );
		glEnd();
	glPopMatrix();

	glBindTexture( GL_TEXTURE_2D, 0 );

	glDisable( GL_ALPHA_TEST );
	glDisable( GL_BLEND );
}

// Draw the complete world
GLint DrawWorld( GLvoid )
{
	glLoadIdentity();

	// Default translation
	g_fXScroll = -g_Camera.pivot->body->p.x;
	glTranslatef( g_fXScroll, -1.0f, -4.7f );

	// For every active shape in world, draw it
	cpSpaceHashEach( space->activeShapes, &DrawActiveShapes, NULL );

	// Draw front of heightmap
	glBindTexture( GL_TEXTURE_2D, textures[1] );

	glBindBuffer( GL_ARRAY_BUFFER, buffers[0] );
	glVertexPointer( 3, GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(0) );
	glNormalPointer( GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(12) );
	glTexCoordPointer( 2, GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(24) );
	
	glDrawArrays( GL_TRIANGLE_STRIP, 0, TERRAIN_SEGMENTS * 2 );

	// Draw top of heightmap
	glBindBuffer( GL_ARRAY_BUFFER, buffers[1] );
	glVertexPointer( 3, GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(0) );
	glNormalPointer( GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(12) );
	glTexCoordPointer( 2, GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(24) );
	
	glDrawArrays( GL_TRIANGLE_STRIP, 0, TERRAIN_SEGMENTS * 2 );

	glPushMatrix();
		glTranslatef( 0.0f, 0.0f, -1.0f );

		glBindTexture( GL_TEXTURE_2D, textures[1] );

		glBindBuffer( GL_ARRAY_BUFFER, buffers[2] );
		glVertexPointer( 3, GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(0) );
		glNormalPointer( GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(12) );
		glTexCoordPointer( 2, GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(24) );
	
		glDrawArrays( GL_TRIANGLE_STRIP, 0, TERRAIN_SEGMENTS * 2 );

		glBindBuffer( GL_ARRAY_BUFFER, buffers[3] );
		glVertexPointer( 3, GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(0) );
		glNormalPointer( GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(12) );
		glTexCoordPointer( 2, GL_FLOAT, sizeof( VertexData ), BUFFER_OFFSET(24) );
	
		glDrawArrays( GL_TRIANGLE_STRIP, 0, TERRAIN_SEGMENTS * 2 );
	glPopMatrix();

	glBindTexture( GL_TEXTURE_2D, 0 );

	float x, y;

	srand( 10 );
	for( int i = 0 ; i < TERRAIN_SEGMENTS ; i += 3 )
	{
		x = i + ((rand() % 30 ) * 0.7f);
		y = 3.0f - (rand() % 10) * 0.1f;
		DrawCloud( x, y );
	}

	return TRUE;
}

// Print text to the screen
GLvoid glPrintf( const char* szFormat, ... )
{
	char	text[256];
	va_list pArguments;

	// No text, do nothing
	if ( szFormat == NULL )
	{
		return;
	}

	va_start( pArguments, szFormat );
	    wvsprintf( text, szFormat, pArguments );
	va_end( pArguments );

	glPushAttrib( GL_LIST_BIT );	
	glListBase( fontList - 32 );
	glCallLists( strlen( text ), GL_UNSIGNED_BYTE, text );
	glPopAttrib();
}

// Draw the complete scene
GLint DrawGLScene( GLvoid )
{
	cpBody * pPcCar;

	// Diffuse light position, following path
	const GLfloat diffusePosition[]  = { 2.8f, 10.0f, 10.0f, 1.0f };

	const GLfloat diffuseColor[] = { 0.65f, 0.65f, 0.65f, 1.0f };

	// Make sure not to scroll further than left and right boundaries
	pPcCar = g_pVehicles[0].chassis->body;
	if( pPcCar->p.x > (g_fTerrainStep * 10) && pPcCar->p.x < (g_fTerrainStep * 191))
	{
		g_fXScroll = pPcCar->p.x * -1.0f;
	}

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glEnableClientState( GL_NORMAL_ARRAY );

	glEnable( GL_DEPTH_TEST );

	glEnable( GL_LIGHT0 );
	glLightfv( GL_LIGHT0, GL_POSITION, diffusePosition );
	glLightfv( GL_LIGHT0, GL_DIFFUSE, diffuseColor );

	//Render world to FBO for use with fragment shaders
	glBindFramebufferEXT( GL_FRAMEBUFFER_EXT, fbo );

	//Bind gl_FragData[0] to GL_COLOR_ATTACHMENT0_EXT and gl_FragData[1] to GL_COLOR_ATTACHMENT1_EXT
	GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0_EXT, GL_COLOR_ATTACHMENT1_EXT };
	glDrawBuffers( 2, drawBuffers );

	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	//Render world with textures
	glUseProgram( programLighting );
		glUniform1i( glGetUniformLocation( programLighting, "tex" ), 0 );
		glEnable( GL_TEXTURE_2D );
		DrawWorld();
		glDisable( GL_TEXTURE_2D );
	glUseProgram( 0 );

	glBindFramebufferEXT( GL_FRAMEBUFFER_EXT, 0 );
	glDrawBuffer( GL_BACK );
	//End of FBO rendering

	glDisable( GL_LIGHT0 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_NORMAL_ARRAY );

	glDisable( GL_DEPTH_TEST );

	//Render program edges
	glMatrixMode( GL_PROJECTION );
	glPushMatrix();
		glLoadIdentity();
		gluOrtho2D( 0, 1, 0, 1 );
		
		glMatrixMode( GL_MODELVIEW );
		glPushMatrix();
			glLoadIdentity();
			
			// 1
			glUseProgram( programEdge );
			
			glEnable( GL_TEXTURE_2D );

			// 2
			glActiveTexture( GL_TEXTURE0 ); glBindTexture( GL_TEXTURE_2D, textureColor );
			glActiveTexture( GL_TEXTURE1 ); glBindTexture( GL_TEXTURE_2D, textureNormal );
			glActiveTexture( GL_TEXTURE2 ); glBindTexture( GL_TEXTURE_2D, textureDepth );
			glUniform1i( glGetUniformLocation( programEdge, "texColor" ), 0 );
			glUniform1i( glGetUniformLocation( programEdge, "texNormal" ), 1 );
			glUniform1i( glGetUniformLocation( programEdge, "texDepth" ), 2 );

			// 3
			glBegin( GL_QUADS );
				glTexCoord2f( 0.0f, 0.0f ); 
				glVertex2f( 0.0f, 0.0f );
				glTexCoord2f( 1.0f, 0.0f );
				glVertex2f( 1.0f, 0.0f );
				glTexCoord2f( 1.0f, 1.0f );
				glVertex2f( 1.0f, 1.0f );
				glTexCoord2f( 0.0f, 1.0f );
				glVertex2f( 0.0f, 1.0f );
			glEnd();

			glActiveTexture( GL_TEXTURE2 ); glBindTexture( GL_TEXTURE_2D, 0 );
			glActiveTexture( GL_TEXTURE1 ); glBindTexture( GL_TEXTURE_2D, 0 );
			glActiveTexture( GL_TEXTURE0 ); glBindTexture( GL_TEXTURE_2D, 0 );

			glDisable( GL_TEXTURE_2D );

			glUseProgram( 0 );

		glPopMatrix();
		glMatrixMode( GL_PROJECTION );
	glPopMatrix();
	glMatrixMode( GL_MODELVIEW );

	//Render score and multiplier text
	glLoadIdentity();
	glTranslatef( 0.0f, 0.0f, -1.0f );

	glColor3ub( 255, 255, 255 );

	glRasterPos2f( -0.4f, 0.38f );
 	glPrintf( "Score: %d", g_nScore );

	glRasterPos2f( -0.4f, 0.35f );
 	glPrintf( "Multiplier: %dx", int( g_fMultiplier ) );

	glRasterPos2f( -0.4f, 0.32f );
 	glPrintf( "Level: %d", int( g_nLevel ) );

	UpdateSpace();

	return TRUE;
}

///***********************************************************///

///***********************************************************///
/// Shader initialization
///***********************************************************///

//Create a shader with given type, size and store it in data pointed to
GLint CreateShader( GLenum shaderType, GLint dataSize, const GLchar* data )
{
	GLuint shader = glCreateShader( shaderType );

	if( shader )
	{
		glShaderSource( shader, 1, &data, &dataSize );
		glCompileShader( shader );

		GLint status = GL_FALSE;
		glGetShaderiv( shader, GL_COMPILE_STATUS, &status );

		if( status == GL_FALSE )
		{
			GLchar log[1024];
			GLint logsize;

			glGetShaderInfoLog( shader, 1024, &logsize, log );

			MessageBox( NULL, "Failed to create shaders, your hardware seems to be unsupported.", "ERROR", MB_OK | MB_ICONEXCLAMATION );
			
#ifdef MEAN
			return 0;
#endif
		}
	}

	return shader;
}

GLint CreateProgram( GLint vertexSize, const GLchar* vertexShader, GLint fragmentSize, const GLchar* fragmentShader )
{
	GLint vShader, fShader, program = 0;

	vShader = CreateShader( GL_VERTEX_SHADER, vertexSize, vertexShader );	
	fShader = CreateShader( GL_FRAGMENT_SHADER, fragmentSize, fragmentShader );

	if( vShader && fShader )
	{
		program = glCreateProgram();
		glAttachShader( program, vShader );
		glAttachShader( program, fShader );
		glLinkProgram( program );
	}

	return program;
}
///***********************************************************///

///***********************************************************///
/// Texture initialization
///***********************************************************///

// Binds the data of a given .tar file to position in data to be
// bound to texture
// See:  http://local.wasp.uwa.edu.au/~pbourke/dataformats/tga/
// Note: Only reads uncompressed .tar files, data is BGRA
bool BindTGAData( char* szFileName, int nDataPosition )
{
	HANDLE File;
	short int w, h;
	DWORD r;

	if( sizeof( int ) != 4 )
	{
		return FALSE;
	}

	if( ( File = CreateFile( szFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL ) ) == INVALID_HANDLE_VALUE )
	{
		return false;
	}

	SetFilePointer( File, 12, 0, FILE_BEGIN );
	ReadFile( File, &w, sizeof( w ), &r, NULL );
	ReadFile( File, &h, sizeof( h ), &r, NULL );

	SetFilePointer( File, 18, 0, FILE_BEGIN );
	ReadFile( File, data[nDataPosition], w * h * 4, &r, NULL );

	CloseHandle( File );

	return true;
}

// Creates a checkboard-like pattern and binds it to position in data
void BindCheckImage( int nDataPosition )
{
   int c;
    
   for ( int y = 0 ; y < 256 ; ++y ) 
   {
      for ( int x = 0 ; x < 256 ; ++x ) 
	  {
         c = (( (( (x & 0x8) == 0) ^ ( (y & 0x8) == 0 )) )) * 255;
         data[nDataPosition][y][x][2] = (GLubyte) c;
         data[nDataPosition][y][x][1] = (GLubyte) c;
         data[nDataPosition][y][x][0] = (GLubyte) c;
		 data[nDataPosition][y][x][3] = (GLubyte) 255;
      }
   }
}

// Creates a grass-like pattern and binds it to position in data
void BindGrassImage( int nValue, int nDataPosition )
{
	for ( int x = 0; x < 256 ; ++x )
	{
		for ( int y = 0; y < 256 ; ++y )
		{
			GLubyte color = (GLubyte) (128.0 + (40.0 * rand()) / RAND_MAX);

			data[nDataPosition][y][x][2] = 0;
			data[nDataPosition][y][x][1] = color * ( float( nValue ) / 100.0f);
			data[nDataPosition][y][x][0] = 0;
			data[nDataPosition][y][x][3] = 255;
		}
	}
}

// Load all textures
GLint LoadGLTextures( GLvoid )
{
	BindCheckImage( 0 );
	BindGrassImage( 93, 1 );
	BindTGAData( "cloud.tga", 2 );

	if ( data != NULL )
	{
		int nCountTextures = sizeof( data ) / sizeof( *data );

		glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );

		glGenTextures( nCountTextures, textures );

		//Check texture
		glBindTexture( GL_TEXTURE_2D, textures[0] );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST );
		gluBuild2DMipmaps( GL_TEXTURE_2D, GL_RGBA, 256, 256, GL_BGRA, GL_UNSIGNED_BYTE, data[0] );

		//Green noise (mountain)
		glBindTexture( GL_TEXTURE_2D, textures[1] );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST );
		gluBuild2DMipmaps( GL_TEXTURE_2D, GL_RGBA, 256, 256, GL_BGRA, GL_UNSIGNED_BYTE, data[1] );

		//Cloud
		glBindTexture( GL_TEXTURE_2D, textures[2] );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST );
		gluBuild2DMipmaps( GL_TEXTURE_2D, GL_RGBA, 256, 256, GL_BGRA, GL_UNSIGNED_BYTE, data[2] );

		return TRUE;
	}

	return FALSE;
}

///***********************************************************///

///***********************************************************///
/// OpenGL initialization
///***********************************************************///

// Initialize OpenGL extensions
GLint InitExtensions( GLvoid )
{
	GLenum err = glewInit();

	if( err != GLEW_OK )
		return FALSE;

	GLchar* extensions = (GLchar*) glGetString( GL_EXTENSIONS );
  
	if ( strstr( extensions, "WGL_EXT_swap_control" ) )
	{
		wglSwapIntervalEXT    = (PFNWGLEXTSWAPCONTROLPROC) wglGetProcAddress( "wglSwapIntervalEXT" );
		wglGetSwapIntervalEXT = (PFNWGLEXTGETSWAPINTERVALPROC) wglGetProcAddress( "wglGetSwapIntervalEXT" );
	}
	else
	{
		MessageBox( NULL, "Failed to initialize vsync, please enable vsync in your driver settings.", "ERROR", MB_OK | MB_ICONEXCLAMATION );
	}

	// Check for FBO and depth texture support
	if( !glewIsSupported( "GL_EXT_framebuffer_object GL_ARB_depth_texture" ) )
		return FALSE;

   return TRUE;
}

// Initialize buffers for screen with dimensions given
GLint InitBuffers( GLsizei nScreenWidth, GLsizei nScreenHeight )
{
	glBindFramebufferEXT( GL_FRAMEBUFFER_EXT, fbo );

	glBindTexture( GL_TEXTURE_2D, textureDepth );
	glTexParameteri( GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, nScreenWidth, nScreenHeight, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, NULL );

	glFramebufferTexture2DEXT( GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, textureDepth, 0 );

	glBindTexture( GL_TEXTURE_2D, textureColor );
	glTexParameteri( GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, nScreenWidth, nScreenHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL );

	glFramebufferTexture2DEXT( GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, textureColor, 0 );

	glBindTexture( GL_TEXTURE_2D, textureNormal );
	glTexParameteri( GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, nScreenWidth, nScreenHeight, 0, GL_RGBA, GL_FLOAT, NULL );

	glFramebufferTexture2DEXT( GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT1_EXT, GL_TEXTURE_2D, textureNormal, 0 );

	GLenum status = glCheckFramebufferStatusEXT( GL_FRAMEBUFFER_EXT );

	if( status != GL_FRAMEBUFFER_COMPLETE_EXT ) {
		return FALSE;
	}

	glBindFramebufferEXT( GL_FRAMEBUFFER_EXT, 0 );

	return TRUE;
}

// Create font and store for use in glPrintf
GLvoid CreateFont( GLvoid )
{
	HFONT	font;
	HFONT	oldfont;

	fontList = glGenLists( 96 );

	font = CreateFont( -12,
						0,
						0,
						0,
						FW_BOLD,
						FALSE,
						FALSE,
						FALSE,
						ANSI_CHARSET,
						OUT_TT_PRECIS,
						CLIP_DEFAULT_PRECIS,
						ANTIALIASED_QUALITY,
						FF_DONTCARE|DEFAULT_PITCH,
						"Verdana" );

	oldfont = (HFONT) SelectObject( hDC, font );
	wglUseFontBitmaps( hDC, 32, 96, fontList );
	SelectObject( hDC, oldfont );
	DeleteObject( font );
}

// Delete the stored font
GLvoid DeleteFont( GLvoid )
{
	glDeleteLists( fontList, 96 );
}

// Initialize OpenGL
GLint InitGL( GLvoid )
{
	if( !LoadGLTextures() )
	{
		return FALSE;
	}

	if( !InitExtensions() )
	{
		return FALSE;
	}

	// Lighting shaders
	if( !(programLighting = CreateProgram( sizeof(vertexShaderDefault), vertexShaderDefault, sizeof(fragmentShaderScene), fragmentShaderScene )) )
	{
#ifdef MEAN
		return FALSE;
#endif
	}

	// Edge shaders
	if( !(programEdge = CreateProgram( sizeof(vertexShaderDefault), vertexShaderDefault, sizeof(fragmentShaderEdge), fragmentShaderEdge )) )
	{
#ifdef MEAN
		return FALSE;
#endif
	}

	glGenFramebuffersEXT( 1, &fbo );
	glGenTextures( 1, &textureDepth );
	glGenTextures( 1, &textureColor );
	glGenTextures( 1, &textureNormal );
	
	glShadeModel( GL_SMOOTH );
	glClearColor( 0.06f, 0.7f, 0.9f, 1 );
	glClearDepth( 1.0f );
	glDepthFunc( GL_LESS );
	glHint( GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST );
	glHint( GL_LINE_SMOOTH_HINT, GL_NICEST );

	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glDisable( GL_COLOR_MATERIAL );

	CreateFont();

	return TRUE;
}

///***********************************************************///

///***********************************************************///
/// World initialization
///***********************************************************///

void CameraPositionSync( cpBody* body, cpFloat dt )
{
	// Sync to player character
	if( g_pVehicles[0].chassis )
		body->p = g_pVehicles[0].chassis->body->p;
}

void ResetArrays( void )
{
	g_nVehicles = 0;
	g_nRocks = 0;
	ZeroMemory( g_pVehicles, sizeof( VehicleData ) * MAX_VEHICLES );
	ZeroMemory( g_pRocks, sizeof( RockData ) * MAX_ROCKS );
}

void AllocArrays( void )
{
	g_pVehicles = new VehicleData[MAX_VEHICLES];
	g_pRocks = new RockData[MAX_ROCKS];

	ResetArrays();
}

void FreeArrays( void )
{
	delete [] g_pVehicles;
	delete [] g_pRocks;
}

cpShape* SpawnVehicle( const int x, unsigned char nCarType = 0, int nWheelPairs = 2, bool bNpc = true )
{
	cpBody*			body;
	cpShape*		shape;
	cpShape*		chassis;

	cpBody*			wheel;
	cpConstraint*	joint;
	cpConstraint*	spring;

	VehicleData*	pVehicleData;
	WheelData*		pWheelData;

	// Make sure that wheel pairs is below the maximum for a vehicle
	if( nWheelPairs > MAX_VEHICLE_WHEELS ) nWheelPairs = MAX_VEHICLE_WHEELS;

	// Get a unique group ID for this vehicle
	int group = ( bNpc ? GROUP_NPC + g_nNpcVehicles : GROUP_PC );

	cpFloat wheelMass = WORLD_SCALE;
	cpVect offset;

	cpVect verts[] = {
		cpv(  WORLD_SCALE, -WORLD_SCALE ),
		cpv( -WORLD_SCALE, -WORLD_SCALE ),
		cpv( -WORLD_SCALE,  WORLD_SCALE * 0.5f ),
		cpv(  WORLD_SCALE,  WORLD_SCALE * 0.5f )
	};

	const cpFloat fWheelRadius = WORLD_SCALE * 0.4f;

	body    = cpBodyNew( WORLD_SCALE * 12.0f, cpMomentForPoly( WORLD_SCALE * 5.0f, sizeof( verts ) / sizeof( cpVect ), verts, cpvzero ) );
	body->p = cpv( x * g_fTerrainStep, g_pRoadHeightMap[x].y + WORLD_SCALE * 3.0f );
	body->w_limit = PLAYER_W_LIMIT;

	cpBodyApplyForce( body, cpv( 0.0f, WORLD_SCALE * 21.0f ), cpvzero );

	if( bNpc )
	{
		// Non-player character vehicle
		++g_nNpcVehicles;
	}
	else
	{
		// Player character vehicle
		++g_nPcVehicles;
	}
	pVehicleData = &g_pVehicles[g_nVehicles++];

	chassis = cpPolyShapeNew( body, sizeof( verts ) / sizeof( cpVect ), verts, cpvzero );
	chassis->e = 0.0f; chassis->u = WORLD_SCALE * 0.5f;
	chassis->group = group;
	chassis->collision_type = T_CHASSIS;
	chassis->data = pVehicleData;
	chassis->layers = LAYER_DEFAULT;
	AddBody( space, chassis, NULL );

	// Camera constraints
	cpSpaceAddConstraint( space, cpSlideJointNew( g_Camera.pivot->body, g_Camera.player->body, cpvzero, cpvzero, CAMERA_FOLLOW_MIN, CAMERA_FOLLOW_MAX ) );
	
	pVehicleData->carType = nCarType;
	pVehicleData->npc = bNpc;
	pVehicleData->chassis = chassis;

	for( int i = 0 ; i < nWheelPairs ; ++i ) 
	{
		offset = cpv( -WORLD_SCALE + (WORLD_SCALE * ( nCarType == 0 && !bNpc ? 3 : 2 ) / float( nWheelPairs - 1 )) * i, -WORLD_SCALE * 1.2f );

		wheel = cpBodyNew( wheelMass, cpMomentForCircle( wheelMass, 0.0, fWheelRadius, cpvzero ) );
		wheel->p = cpvadd( body->p, offset );
		wheel->v = body->v;

		shape = cpCircleShapeNew( wheel, fWheelRadius, cpvzero );
		shape->e = 0.0;
		shape->u = 2.5;
		
		if( nCarType == 0 )
		{
			shape->collision_type = T_WHEEL;
		}
		else
		{
			shape->collision_type = T_WHEEL_TRAILER;
		}
		
		shape->group = group;
		shape->data = pVehicleData;
		shape->layers = LAYER_DEFAULT;
		AddBody( space, shape, NULL );

		// Create a joint that holds the wheel
		joint = cpPinJointNew( body, wheel, cpvzero, cpvzero );
		cpSpaceAddConstraint( space, joint );

		// Create a spring for wheel suspension
		spring = cpDampedSpringNew( body, wheel, cpv( offset.x, offset.y ), cpvzero, 0.0f, 300.0f, WORLD_SCALE );
		cpSpaceAddConstraint( space, spring );

		pWheelData = &pVehicleData->wheel[i];
		pWheelData->attached = true;
		pWheelData->joint = joint;
		pWheelData->spring = spring;
		pWheelData->wheel = shape;
	}

	cpBodySetAngle( body, -g_pRoadHeightMap[x].a );

	return chassis;
}

// Creates a new rock and adds it to the physics space
void SpawnRock()
{
	cpBody*  body;
	cpShape* shape;

	RockData* pRockData;

	const cpFloat radius = 0.1f + float( rand() % 20 ) / 25.0f * 0.4f;
	const cpFloat mass   = radius * WORLD_SCALE * 75.0f;

	int x = rand() % TERRAIN_SEGMENTS;

	body       = cpBodyNew( mass, cpMomentForCircle( mass, 0.0f, radius, cpvzero ) );
	body->p    = cpv( x * g_fTerrainStep, g_pRoadHeightMap[x].y + WORLD_SCALE * 3.0f + radius );

	pRockData = &g_pRocks[g_nRocks++];
	shape    = cpCircleShapeNew( body, radius, cpvzero );
	shape->e = 0.05f; shape->u = 0.1f;
	shape->data = pRockData;
	shape->collision_type = T_ROCK;
	shape->layers = LAYER_DEFAULT;
	AddBody( space, shape, NULL );

	pRockData->rock = shape;
}

void StartLevel()
{
	cpConstraint* constraint;
	cpShape* shape;

	cpBody* body;
	cpBody* bodyLast;

	int f = 0 + 10;

	ResetArrays();

	// Spawn player character vehicle (car)
	shape = SpawnVehicle( f + 2, 0, COUNT_WHEELS_CAR, false );
	g_pVehicles[0].chassis = shape;
	body = bodyLast = shape->body;
	
	// Spawn player character vehicles (trailers)
	for( int i = 0; i < g_nLevel; ++i )
	{
		shape = SpawnVehicle( f - i * 2, 1, COUNT_WHEELS_TRAILER, false );
		body = shape->body;

		constraint = cpDampedSpringNew( body, bodyLast, cpvzero, cpvzero, bodyLast->p.x - body->p.x, 600.0f, 1.0f );
		g_pVehicles[g_nVehicles - 1].link = constraint;

		cpSpaceAddConstraint( space, constraint );
		cpSpaceAddConstraint( space, cpRotaryLimitJointNew( body, bodyLast, -5.0f * (M_PI / 180), 5.0f * (M_PI / 180) ) );
		cpSpaceAddConstraint( space, cpGrooveJointNew( bodyLast, body, cpv( -WORLD_SCALE * 10, 0 ), cpvzero, cpvzero ) );

		bodyLast = body;
	}

	float c = (TERRAIN_SEGMENTS - 2 * f) / (g_nLevel * 2);

	// Spawn non-player character vehicles
	for( int i = 0; i < (g_nLevel * 2); ++i )
	{
		f += c;
		SpawnVehicle( f, 0 );
	}

	// Spawn rocks
	srand( GetTickCount() );
	for( int i = 0 ; i < (g_nLevel * 3) ; ++i )
	{
		SpawnRock();
	}
}

static void NewSpace( cpSpace *space, cpShape *shape, void *data )
{
	int i, j;
	WheelData * pWheelData;

	// Removes the rocks
	for( i = 0 ; i < g_nRocks ; ++i )
	{
		RemoveBody( space, g_pRocks[i].rock, NULL );
	}

	// Removes the vehicles
	for( i = 0 ; i < g_nVehicles ; ++i )
	{
		RemoveBody( space, g_pVehicles[i].chassis, NULL );
		for( j = 0 ; j < MAX_VEHICLE_WHEELS ; ++j )
		{
			pWheelData = &g_pVehicles[i].wheel[j];
			if( pWheelData->wheel != NULL )
			{
				// Existing wheel, remove it
				RemoveBody( space, pWheelData->wheel, NULL );
			}
		}
	}

	++g_nLevel;

	StartLevel();
}

static int NewLevel( cpArbiter* pArbiter, struct cpSpace* pSpace, void* pData )
{    
	cpSpaceAddPostStepCallback( space, (cpPostStepFunc) NewSpace, NULL, NULL );

    return FALSE;
}

// Initialize globals
void InitGlobals( void )
{
	// Initialize global variables
	g_nNpcVehicles = 0;
	g_nPcVehicles = 0;
	g_fBoost = 0;
	g_bKeyLock = false;
	g_bActiveWindow = true;
	g_nScore = 0;
	g_fMultiplier = 1.0f;
	g_dwLastScoreTime = GetTickCount();
	g_nLevel = 1;
}

// Destroys the physics space
void DestroyWorld( void )
{
	FreeArrays();
}

// Initialize the physics space
void InitWorld( void )
{
	cpFloat x = 0.0f, y = 0.0f, xBuf = 0.0f, yBuf = 0.0f, angle = 0.0f;

	cpBody* body;
	cpShape* shape;
	cpConstraint* constraint;

	VertexData* roadFront = new VertexData[TERRAIN_SEGMENTS * 2];
	VertexData* roadTop   = new VertexData[TERRAIN_SEGMENTS * 2];
	VertexData* mountainFront = new VertexData[TERRAIN_SEGMENTS * 2];
	VertexData* mountainTop   = new VertexData[TERRAIN_SEGMENTS * 2];
	g_pRoadHeightMap = new HeightData[TERRAIN_SEGMENTS];
	g_pMountainHeightMap = new HeightData[TERRAIN_SEGMENTS];

	AllocArrays();

	//Clear and create a new space
	cpResetShapeIdCounter();
	space = cpSpaceNew();
	space->iterations = 20;
	cpSpaceResizeActiveHash( space, 0.5f, 150 );
	space->gravity = cpv(0, -5.0f);

	glGenBuffers( 4, buffers );

	bounds = cpBodyNew( INFINITY, INFINITY );
	bounds->p = cpv( 0.0f, 0.0f );

	g_fTerrainStep = float( TERRAIN_WIDTH ) / float( TERRAIN_SEGMENTS );

	cpSpaceResizeStaticHash( space, g_fTerrainStep, 40 );

	// Build heightmap
	float period = 0.5f;
	for( int i = 0 ; i < TERRAIN_SEGMENTS ; ++i )
	{
		float p = float( i ) / float( TERRAIN_SEGMENTS ) * 20.0f * M_PI;
		y = sin( p * period ) * cos( (p) * 0.1f * period );
		x = float( i ) * g_fTerrainStep;

		roadFront[i * 2].x  = x;
		roadFront[i * 2].y  = -1.0f;
		roadFront[i * 2].z  = 0.0f;
		roadFront[i * 2].nx = 0.0f;
		roadFront[i * 2].ny = 0.0f;
		roadFront[i * 2].nz = 1.0f;
		roadFront[i * 2].s  = x;
		roadFront[i * 2].t  = 0.0f;

		roadFront[i * 2 + 1].x  = x;
		roadFront[i * 2 + 1].y  = y;
		roadFront[i * 2 + 1].z  = 0.0f;
		roadFront[i * 2 + 1].nx = 0.0f;
		roadFront[i * 2 + 1].ny = 0.0f;
		roadFront[i * 2 + 1].nz = 1.0f;
		roadFront[i * 2 + 1].s  = x;
		roadFront[i * 2 + 1].t  = 1.0f;

		angle = (i > 0 ? tan( (yBuf - y) / (x - xBuf) ) : 0);
		roadTop[i * 2].x  = x;
		roadTop[i * 2].y  = y;
		roadTop[i * 2].z  = 0.0f;
		roadTop[i * 2].nx = cos( angle );
		roadTop[i * 2].ny = sin( angle );
		roadTop[i * 2].nz = 0.0f;
		roadTop[i * 2].s  = x;
		roadTop[i * 2].t  = 0.0f;

		roadTop[i * 2 + 1].x  = x;
		roadTop[i * 2 + 1].y  = y;
		roadTop[i * 2 + 1].z  = -1.0f;
		roadTop[i * 2 + 1].nx = cos( angle );
		roadTop[i * 2 + 1].ny = sin( angle );
		roadTop[i * 2 + 1].nz = 0.0f;
		roadTop[i * 2 + 1].s  = x;
		roadTop[i * 2 + 1].t  = 1.0f;

		g_pRoadHeightMap[i].y = y;
		g_pRoadHeightMap[i].a = angle;

		if( i > 0)
		{
			shape = cpSegmentShapeNew( bounds, cpv( xBuf, yBuf ), cpv( x, y ), 0.0f );
			shape->e = 0.0f; shape->u = 1.0f;
			shape->collision_type = T_ROAD;
			shape->layers = LAYER_DEFAULT;
			cpSpaceAddStaticShape( space, shape );
		}

		xBuf = x;
		yBuf = y;
	}

	period = 1.8f;

	for( int i = 0 ; i < TERRAIN_SEGMENTS ; ++i )
	{
		float p = float( i ) / float( TERRAIN_SEGMENTS ) * 20.0f * M_PI;
		y = 1.5f + sin( p * period ) * cos( (p) * 0.1f * period );
		x = float( i ) * g_fTerrainStep;

		mountainFront[i * 2].x  = x;
		mountainFront[i * 2].y  = -1.5f;
		mountainFront[i * 2].z  = -1.0f;
		mountainFront[i * 2].nx = 0.0f;
		mountainFront[i * 2].ny = 0.0f;
		mountainFront[i * 2].nz = 1.0f;
		mountainFront[i * 2].s  = x;
		mountainFront[i * 2].t  = 0.0f;

		mountainFront[i * 2 + 1].x  = x;
		mountainFront[i * 2 + 1].y  = y;
		mountainFront[i * 2 + 1].z  = 0.0f;
		mountainFront[i * 2 + 1].nx = 0.0f;
		mountainFront[i * 2 + 1].ny = 0.0f;
		mountainFront[i * 2 + 1].nz = 1.0f;
		mountainFront[i * 2 + 1].s  = x;
		mountainFront[i * 2 + 1].t  = 1.0f;

		angle = (i > 0 ? tan( (yBuf - y) / (x - xBuf) ) : 0);
		mountainTop[i * 2].x  = x;
		mountainTop[i * 2].y  = y;
		mountainTop[i * 2].z  = 0.0f;
		mountainTop[i * 2].nx = cos( angle );
		mountainTop[i * 2].ny = sin( angle );
		mountainTop[i * 2].nz = 0.0f;
		mountainTop[i * 2].s  = x;
		mountainTop[i * 2].t  = 0.0f;

		mountainTop[i * 2 + 1].x  = x;
		mountainTop[i * 2 + 1].y  = y;
		mountainTop[i * 2 + 1].z  = -1.0f;
		mountainTop[i * 2 + 1].nx = cos( angle );
		mountainTop[i * 2 + 1].ny = sin( angle );
		mountainTop[i * 2 + 1].nz = 0.0f;
		mountainTop[i * 2 + 1].s  = x;
		mountainTop[i * 2 + 1].t  = 1.0f;

		g_pMountainHeightMap[i].y = y;
		g_pMountainHeightMap[i].a = angle;

		if( i > 0)
		{
			shape = cpSegmentShapeNew( bounds, cpv( xBuf, yBuf ), cpv( x, y ), 0.0f );
			shape->e = 0.0f; shape->u = 1.0f;
			shape->sensor = TRUE;
			shape->collision_type = T_MOUNTAIN;
			cpSpaceAddStaticShape( space, shape );
		}

		xBuf = x;
		yBuf = y;
	}

	// Add camera physics to world
	body = cpBodyNew( 10.0f, cpMomentForCircle( 10.0f, 0.0, WORLD_SCALE, cpvzero ) );
	body->p = cpv( 0, 0 );
	body->v_limit = 0;
	cpSpaceAddBody( space, body );

	shape = cpCircleShapeNew( body, WORLD_SCALE, cpvzero );
	shape->sensor = TRUE;
	g_Camera.pivot = cpSpaceAddShape( space, shape );
	constraint = cpGrooveJointNew( bounds, body, cpv( 0, 0 ), cpv( TERRAIN_WIDTH, 0 ), cpv( 0, 0 ) );

	body = cpBodyNew( INFINITE, INFINITE );
	body->p = cpv( 0, 0 );
	body->v_limit = 0;
	body->position_func = CameraPositionSync;
	cpSpaceAddBody( space, body );

	shape = cpCircleShapeNew( body, WORLD_SCALE, cpvzero );
	shape->sensor = TRUE;
	g_Camera.player = cpSpaceAddShape( space, shape );

	// Upload bufferdata to vertexbuffer
	glBindBuffer( GL_ARRAY_BUFFER, buffers[0] );
	glBufferData( GL_ARRAY_BUFFER, (TERRAIN_SEGMENTS + 1) * sizeof( VertexData ) * 2, roadFront, GL_STATIC_DRAW );
	
	glBindBuffer( GL_ARRAY_BUFFER, buffers[1] );
	glBufferData( GL_ARRAY_BUFFER, (TERRAIN_SEGMENTS + 1) * sizeof( VertexData ) * 2, roadTop, GL_STATIC_DRAW );

	glBindBuffer( GL_ARRAY_BUFFER, buffers[2] );
	glBufferData( GL_ARRAY_BUFFER, (TERRAIN_SEGMENTS + 1) * sizeof( VertexData ) * 2, mountainFront, GL_STATIC_DRAW );
	
	glBindBuffer( GL_ARRAY_BUFFER, buffers[3] );
	glBufferData( GL_ARRAY_BUFFER, (TERRAIN_SEGMENTS + 1) * sizeof(VertexData ) * 2, mountainTop, GL_STATIC_DRAW );

	// Left and right boundaries
	shape = cpSegmentShapeNew( bounds, cpv( 1 * g_fTerrainStep, -5.0f ), cpv( 1* g_fTerrainStep, 10.0f ), 0.0f );
	shape->e = 0.0f; shape->u = 10.0f;
	shape->collision_type = T_LEFT_BOUNDARY;
	cpSpaceAddStaticShape( space, shape );

	shape = cpSegmentShapeNew( bounds, cpv( 199 * g_fTerrainStep, -5.0f ), cpv( 199 * g_fTerrainStep, 10.0f ), 0.0f );
	shape->e = 0.0f; shape->u = 10.0f;
	shape->collision_type = T_RIGHT_BOUNDARY;
	cpSpaceAddStaticShape( space, shape );

	shape = cpSegmentShapeNew( bounds, cpv( 190 * g_fTerrainStep, -5.0f ), cpv( 199 * g_fTerrainStep, 10.0f ), 0.0f );
	shape->e = 0.0f; shape->u = 10.0f;
	shape->collision_type = T_FINISH;
	shape->sensor = TRUE;
	cpSpaceAddStaticShape( space, shape );

	StartLevel();

	// Collision handlers
	cpSpaceAddCollisionHandler( space, T_CHASSIS, T_FINISH,			 NewLevel,				   NULL, NULL, NULL, NULL );
	cpSpaceAddCollisionHandler( space, T_CHASSIS, T_WHEEL_TRAILER,	 KillNpcHandler,		   NULL, NULL, NULL, NULL );
	cpSpaceAddCollisionHandler( space, T_WHEEL,   T_WHEEL_TRAILER,	 KillNpcHandler,		   NULL, NULL, NULL, NULL );
	cpSpaceAddCollisionHandler( space, T_ROCK,    T_WHEEL_TRAILER,	 KillNpcHandler,		   NULL, NULL, NULL, NULL );

	delete[] roadTop;
	delete[] roadFront;
	delete[] mountainTop;
	delete[] mountainFront;
}

///***********************************************************///

///***********************************************************///
/// Window initialization
///***********************************************************///

GLvoid KillGLWindow( GLvoid )
{
	DestroyWorld();

	if( hRC )
	{
		if( !wglMakeCurrent( NULL, NULL ) )
		{
			MessageBox( NULL, "Release Of DC And RC Failed.", "SHUTDOWN ERROR", MB_OK | MB_ICONINFORMATION );
		}

		if( !wglDeleteContext( hRC ) )
		{
			MessageBox( NULL, "Release Rendering Context Failed.", "SHUTDOWN ERROR", MB_OK | MB_ICONINFORMATION );
		}

		hRC = NULL;
	}

	if( hDC && !ReleaseDC( hWnd, hDC ) )
	{
		MessageBox( NULL, "Release Device Context Failed.", "SHUTDOWN ERROR", MB_OK | MB_ICONINFORMATION );
		hDC = NULL;
	}

	if( hWnd && !DestroyWindow( hWnd ) )
	{
		MessageBox( NULL, "Could Not Release hWnd.", "SHUTDOWN ERROR", MB_OK | MB_ICONINFORMATION );
		hWnd = NULL;
	}

	if( !UnregisterClass( "OpenGL", hInstance ) )
	{
		MessageBox( NULL, "Could Not Unregister Class.", "SHUTDOWN ERROR", MB_OK | MB_ICONINFORMATION );
		hInstance = NULL;
	}
}

GLvoid ReSizeGLScene( GLsizei width, GLsizei height )
{
	if ( height == 0 )
	{
		height = 1;
	}

	// Create framebuffers en textures
	InitBuffers( width, height );

	glViewport( 0, 0, width, height );

	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();

	gluPerspective( 45.0f, (GLfloat) width / (GLfloat) height, 0.1f, 10.0f );

	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
}

int CreateGLWindow( char* title, int width, int height, int bits, bool fullscreen )
{
	GLuint		PixelFormat;
	WNDCLASS	wc;		
	DWORD		dwExStyle;
	DWORD		dwStyle;
	RECT		WindowRect;

	WindowRect.left   = (long) 0;
	WindowRect.right  = (long) width;
	WindowRect.top    = (long) 0;
	WindowRect.bottom = (long) height;

	hInstance			= GetModuleHandle( NULL );
	wc.style			= CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wc.lpfnWndProc		= (WNDPROC) WndProc;
	wc.cbClsExtra		= 0;
	wc.cbWndExtra		= 0;
	wc.hInstance		= hInstance;
	wc.hIcon			= LoadIcon( NULL, IDI_WINLOGO );
	wc.hCursor			= LoadCursor( NULL, IDC_ARROW );
	wc.hbrBackground	= NULL;
	wc.lpszMenuName		= NULL;
	wc.lpszClassName	= "OpenGL";

	if( !RegisterClass( &wc ) )
	{
		MessageBox( NULL, "Failed To Register The Window Class.", "ERROR", MB_OK | MB_ICONEXCLAMATION );

		return FALSE;
	}

	if( fullscreen )
	{
		DEVMODE dmScreenSettings;
		ZeroMemory( &dmScreenSettings, sizeof( dmScreenSettings ) );

		dmScreenSettings.dmSize			= sizeof( dmScreenSettings );
		dmScreenSettings.dmPelsWidth	= width;
		dmScreenSettings.dmPelsHeight	= height;
		dmScreenSettings.dmBitsPerPel	= bits;
		dmScreenSettings.dmFields		= DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;

		if( ChangeDisplaySettings( &dmScreenSettings, CDS_FULLSCREEN ) != DISP_CHANGE_SUCCESSFUL )
		{
			if( MessageBox( NULL, "The Requested Fullscreen Mode Is Not Supported By\nYour Video Card. Use Windowed Mode Instead?", "", MB_YESNO | MB_ICONEXCLAMATION ) == IDYES )
			{
				fullscreen = FALSE;
			}
			else
			{
				MessageBox( NULL, "Program Will Now Close.", "ERROR", MB_OK | MB_ICONSTOP );

				return FALSE;
			}
		}
	}

	if( fullscreen )	
	{
		dwExStyle = WS_EX_APPWINDOW;
		dwStyle   = WS_POPUP;
		ShowCursor( FALSE );
	}
	else
	{
		dwExStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
		dwStyle   = WS_OVERLAPPEDWINDOW;
	}

	AdjustWindowRectEx( &WindowRect, dwStyle, FALSE, dwExStyle );

	if( !(hWnd = CreateWindowEx( dwExStyle, 
							    "OpenGL", 
							    title, 
							    dwStyle | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 
							    0, 
						     	0, 
							    WindowRect.right-WindowRect.left, 
							    WindowRect.bottom-WindowRect.top,
							    NULL,
							    NULL,
							    hInstance,
							    NULL )) )
	{
		KillGLWindow();
		MessageBox( NULL, "Failed to create window", "ERROR", MB_OK | MB_ICONEXCLAMATION );

		return FALSE;
	}

	static	PIXELFORMATDESCRIPTOR pfd =
	{
		sizeof(PIXELFORMATDESCRIPTOR),
		1,		
		PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
		PFD_TYPE_RGBA,
		bits,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0,
		PFD_MAIN_PLANE,
		0, 0, 0, 0
	};
	
	if( !(hDC = GetDC( hWnd )) )
	{
		KillGLWindow();
		MessageBox( NULL, "Failed to create OpenGL DC", "ERROR", MB_OK | MB_ICONEXCLAMATION );

		return FALSE;
	}

	if( !(PixelFormat = ChoosePixelFormat( hDC,&pfd )) )
	{
		KillGLWindow();
		MessageBox( NULL, "Failed to choose PixelFormat", "ERROR", MB_OK | MB_ICONEXCLAMATION );

		return FALSE;
	}

	if( !SetPixelFormat( hDC, PixelFormat, &pfd ) )
	{
		KillGLWindow();
		MessageBox( NULL, "Failed to set PixelFormat", "ERROR",MB_OK|MB_ICONEXCLAMATION );

		return FALSE;
	}

	if( !(hRC=wglCreateContext( hDC )) )
	{
		KillGLWindow();
		MessageBox( NULL, "Failed to create OpenGL RC", "ERROR", MB_OK | MB_ICONEXCLAMATION );

		return FALSE;
	}

	if( !wglMakeCurrent( hDC, hRC ) )
	{
		KillGLWindow();
		MessageBox( NULL, "Failed to activate OpenGL RC", "ERROR", MB_OK | MB_ICONEXCLAMATION );
		return FALSE;
	}

	if( !InitGL() )
	{
		KillGLWindow();
		MessageBox( NULL, "Initialization Failed", "ERROR", MB_OK | MB_ICONEXCLAMATION );
		return FALSE;
	}

	ShowWindow( hWnd,SW_SHOW );
	SetForegroundWindow( hWnd );
	SetFocus( hWnd);
	ReSizeGLScene( width, height );

	cpInitChipmunk();

	InitWorld();

	return TRUE;
}
///***********************************************************///

///***********************************************************///
/// Handler and entry-point
///***********************************************************///

LRESULT CALLBACK WndProc( HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam )
{
	switch( uMsg )
	{
		case WM_ACTIVATE:
		{
			if( (LOWORD( wParam ) != WA_INACTIVE) && !((BOOL) HIWORD( wParam )) )
			{
				g_bActiveWindow = true;
			}
			else
			{			
				g_bActiveWindow = false;
			}

			return 0;
		}

		case WM_SYSCOMMAND:
		{
			switch( wParam )
			{
				case SC_SCREENSAVE:
				case SC_MONITORPOWER:

				return 0;
			}

			break;
		}

		case WM_CLOSE:
		{
			PostQuitMessage( 0 );

			return 0;
		}

		case WM_KEYDOWN:
		{
			g_bKeys[wParam] = TRUE;

			return 0;
		}

		case WM_KEYUP:
		{
			g_bKeys[wParam] = FALSE;

			return 0;
		}

		case WM_SIZE:
		{
			ReSizeGLScene( LOWORD(lParam), HIWORD(lParam) );

			return 0;
		}
	}

	return DefWindowProc( hWnd, uMsg, wParam, lParam );
}

int __stdcall WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow )
{
	MSG msg;

	InitGlobals();

	// Make sure width and height are equal and power of 2
	if ( !CreateGLWindow( "NHTV Demo", 512, 512, 32, false ) )
	{
		return 0;
	}

	while( bRun )
	{
		if( PeekMessage( &msg, NULL, 0, 0, PM_REMOVE ) )
		{
			if( msg.message == WM_QUIT )
			{
				bRun = false;
			}
			else
			{
				TranslateMessage( &msg );
				DispatchMessage( &msg );
			}
		}
		else
		{
			if( (g_bActiveWindow && !DrawGLScene()) || g_bKeys[VK_ESCAPE] )
			{
				bRun = false;
			}
			else
			{
				SwapBuffers( hDC );
			}

			if( g_bKeys[VK_SHIFT] )
			{
				HandlePcVehicle( HANDLING_BOOST );
			}

			if( !g_bKeys[VK_SPACE] && g_bKeyLock )
			{
				ShootAxle();
				g_bKeyLock = FALSE;
			}

			if( g_bKeys[VK_SPACE] )
			{
				g_bKeyLock = TRUE;
			}

			if( g_bKeys[VK_LEFT] )
			{
				HandlePcVehicle( HANDLING_BRAKE );
			}

			if( g_bKeys[VK_RIGHT] )
			{
				HandlePcVehicle( HANDLING_ACCELERATE );
			}
		}
	}

	KillGLWindow();
	return msg.wParam;
}

///***********************************************************///
