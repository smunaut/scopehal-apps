#Set up include paths
include_directories(${GTKMM_INCLUDE_DIRS} ${SIGCXX_INCLUDE_DIRS})
link_directories(${GTKMM_LIBRARY_DIRS} ${SIGCXX_LIBRARY_DIRS})

###############################################################################
#C++ compilation
add_executable(glscopeclient
	ChannelPropertiesDialog.cpp
	Framebuffer.cpp
	HistoryWindow.cpp
	MeasurementDialog.cpp
	OscilloscopeWindow.cpp
	Program.cpp
	ProtocolAnalyzerWindow.cpp
	ProtocolDecoderDialog.cpp
	Shader.cpp
	ShaderStorageBuffer.cpp
	Texture.cpp
	Timeline.cpp
	VertexArray.cpp
	VertexBuffer.cpp
	WaveformArea.cpp
	WaveformArea_events.cpp
	WaveformArea_rendering.cpp
	WaveformArea_cairo.cpp
	WaveformGroup.cpp

	main.cpp
)

###############################################################################
#Linker settings
target_link_libraries(glscopeclient
	scopehal
	scopeprotocols
	scopemeasurements
	${GTKMM_LIBRARIES}
	${SIGCXX_LIBRARIES}
	GL
	)

