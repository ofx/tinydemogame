/* Shaders
 *
 * Cel-shading implementation using vertex and fragment shaders.
 *
 * See: http://en.wikipedia.org/wiki/Cel-shaded_animation
 * See: http://en.wikipedia.org/wiki/Sobel_operator
 * See: http://en.wikipedia.org/wiki/Framebuffer_Object
 * See: http://www.geeks3d.com/20091216/geexlab-how-to-visualize-the-depth-buffer-in-glsl/
 *
 * By: Marlon Etheredge <m.etheredge@gmail.com>
 */

const GLchar vertexShaderDefault[] = 
	"varying vec3 vertexNormal;"
	"varying float NdotL;"
	""
	"void main( void )"
	"{"
	"	vertexNormal = normalize( gl_NormalMatrix * gl_Normal );"						// Pass normal
	""
	"	vec4 vertexWorldSpace = gl_ModelViewMatrix * gl_Vertex;"						// Set position in world space
	""																		
	"	vec3 lightDirection = gl_LightSource[0].position.xyz - vertexWorldSpace.xyz;"	// Calculate vertex to light
	"	NdotL = max( dot( vertexNormal, normalize( lightDirection ) ), 0.0 );"
	""
	"	gl_Position = ftransform();"													// Position to camera space
	"	gl_TexCoord[0] = gl_MultiTexCoord0;"											// Pass texture coords
	"}";

const GLchar fragmentShaderScene[] =
	"uniform sampler2D tex;"
	""
	"varying vec3 vertexNormal;"
	"varying float NdotL;"
	""
	"float hardstep( float x )"															// Hardstep light intensity
	"{"
	"	float s;"
	"	if		( x > 0.75 )	s = 1.0; "
	"	else if	( x > 0.50 )	s = 0.5; "
	"	else if	( x > 0.20 )	s = 0.2; "
	"	else					s = 0.1; "
	""
	"	return s;"
	"}"
	""
	"void main( void )"
	"{"
	"	vec4 color;"																	// Final color
	"	float intensity;"																// Diffuse light intensity
	"	float ambient = 0.4;"															// Ambient light intensity
	""
	"	color = texture2D( tex, gl_TexCoord[0].st );"
	""
	"	if( NdotL > 0.0 )"
	"	{"
	"		intensity = NdotL;"
	"	}"
	""
	"	color = color * vec4( gl_LightSource[0].diffuse.xyz, 1 ) * (ambient + hardstep( intensity ));" //Add lighting and hardstep diffuse light intensity
	"	gl_FragData[0] = color;"
	"	gl_FragData[1].rgb = vertexNormal;"
	"}";

const GLchar fragmentShaderEdge[] =
	"uniform sampler2D texColor;"
	"uniform sampler2D texNormal;"
	"uniform sampler2D texDepth;"
	""
	"float LinearizeDepth(float z)"
	"{"
	"	float n = 0.1;"																	// Camera near
	"	float f = 10.0;"																// Camera far
	"	return (2.0 * n) / (f + n - z * (f - n));"
	"}"
	""
	"vec4 getData( vec2 t )"
	"{"
	"	vec4 n;"
	"	n.xyz = -1.0 + texture2D( texNormal, t ).xyz * 2.0;"
	"	n.w = LinearizeDepth( texture2D( texDepth, t ).x ) * 10;"
	"	return n;"
	"}"
	""
	"void main( void )"
	"{"
	"	vec3 color;"
	""
	"	vec3 pixelColor = texture2D( texColor, gl_TexCoord[0].st ).xyz;"
	""
	"	float o = 1.0 / 512;"
	"	vec4 g00,g01,g02, g10,g12, g20,g21,g22;"
	"	g00 = getData( gl_TexCoord[0].st + vec2( -o, -o ) ); "
	"	g01 = getData( gl_TexCoord[0].st + vec2(  0, -o ) ); "
	"	g02 = getData( gl_TexCoord[0].st + vec2( +o, -o ) ); "
	""
	"	g10 = getData( gl_TexCoord[0].st + vec2( -o,  0 ) ); "
	"	g12 = getData( gl_TexCoord[0].st + vec2( +o,  0 ) ); "
	""
	"	g20 = getData( gl_TexCoord[0].st + vec2( -o, +o ) ); "
	"	g21 = getData( gl_TexCoord[0].st + vec2(  0, +o ) ); "
	"	g22 = getData( gl_TexCoord[0].st + vec2( +o, +o ) ); "
	""
	"	vec4 edgeX = g00 + 2 * g10 + g20 - g02 - 2 * g12 - g22;"
	"	vec4 edgeY = g00 + 2 * g01 + g02 - g20 - 2 * g21 - g22;"
	""
	"	vec4 G = edgeX * edgeX + edgeY * edgeY;"
	"	float Gm = dot( G, vec4( 0.4 ) );"
	""
	"	float edge = 1.0 - Gm;"
	"	edge = max( edge, 0.1 );"
	"	color = pixelColor * edge;"
	""	
	"	gl_FragColor = vec4( color, 1 );"
	"}";