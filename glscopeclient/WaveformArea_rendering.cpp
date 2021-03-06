/***********************************************************************************************************************
*                                                                                                                      *
* ANTIKERNEL v0.1                                                                                                      *
*                                                                                                                      *
* Copyright (c) 2012-2020 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief  OpenGL rendering code for WaveformArea
 */

#include "glscopeclient.h"
#include "WaveformArea.h"
#include "OscilloscopeWindow.h"
#include <random>
#include <map>
#include "ProfileBlock.h"
#include "../../lib/scopehal/TextRenderer.h"
#include "../../lib/scopehal/DigitalRenderer.h"
#include "../../lib/scopeprotocols/EyeDecoder2.h"
#include "../../lib/scopeprotocols/WaterfallDecoder.h"

using namespace std;
using namespace glm;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

void WaveformArea::PrepareGeometry(WaveformRenderData* wdata)
{
	double start = GetTime();

	auto channel = wdata->m_channel;
	auto pdat = channel->GetData();
	auto andat = dynamic_cast<AnalogCapture*>(pdat);
	auto digdat = dynamic_cast<DigitalCapture*>(pdat);
	if( !(andat && andat->GetDepth()) && !(digdat && digdat->GetDepth()))
	{
		wdata->m_geometryOK = false;
		return;
	}

	size_t count;
	if(andat)
		count = andat->size();
	else
		count = digdat->size();
	double xscale = pdat->m_timescale * m_group->m_pixelsPerXUnit;
	float xoff = (pdat->m_triggerPhase - m_group->m_xAxisOffset) * m_group->m_pixelsPerXUnit;

	bool fft = IsFFT();

	//Zero voltage level
	//TODO: don't assume all digital data is a protocol decode, logic analyzers are a thing!
	//TODO: properly calculate decoder positions once RenderDecodeOverlays() isn't doing that anymore
	float ybase = m_height/2;
	if(digdat)
		ybase = m_height - (m_overlayPositions[dynamic_cast<ProtocolDecoder*>(channel)] + 15);

	//Calculate X/Y coordinate of each sample point
	//TODO: some of this can probably move to GPU too?
	vector<float> traceBuffer;
	vector<uint32_t> indexBuffer;
	traceBuffer.resize(count*2);
	indexBuffer.resize(m_width);
	double offset = channel->GetOffset();
	#pragma omp parallel for num_threads(8)
	for(size_t j=0; j<count; j++)
	{
		traceBuffer[j*2] = pdat->GetSampleStart(j) * xscale + xoff;

		float y;
		if(digdat)
		{
			//TODO: digital overlay stuff
			y = ybase + 5 + ( (*digdat)[j] ? 20: 0 );
		}
		else if(fft)
			y = DbToYPosition(-70 - (20 * log10((*andat)[j])));		//TODO: don't hard code plot limits
		else
			y = (m_pixelsPerVolt * ((*andat)[j] + offset)) + ybase;

		traceBuffer[j*2 + 1]	= y;
	}

	double dt = GetTime() - start;
	m_prepareTime += dt;
	start = GetTime();

	//Calculate indexes for rendering.
	//This is necessary since samples may be sparse and have arbitrary spacing between them, so we can't
	//trivially map sample indexes to X pixel coordinates.
	//TODO: can we parallelize this? move to a compute shader?
	size_t nsample = 0;
	for(int j=0; j<m_width; j++)
	{
		//Default to drawing nothing
		indexBuffer[j] = count;

		//Move forward until we find a sample that starts in the current column
		for(; nsample < count-1; nsample ++)
		{
			//If the next sample ends after the start of the current pixel. stop
			float end = traceBuffer[(nsample+1)*2];
			if(end >= j)
			{
				//Start the current column at this sample
				indexBuffer[j] = nsample;
				break;
			}
		}
	}

	dt = GetTime() - start;
	m_indexTime += dt;
	start = GetTime();

	//Download it
	wdata->m_waveformStorageBuffer.Bind();
	glBufferData(GL_SHADER_STORAGE_BUFFER, traceBuffer.size()*sizeof(float), &traceBuffer[0], GL_STREAM_DRAW);

	//Config stuff
	uint32_t config[4];
	config[0] = m_height;							//windowHeight
	config[1] = m_plotRight;						//windowWidth
	config[2] = count;								//depth
	config[3] = m_parent->GetTraceAlpha() * 256;	//alpha
	wdata->m_waveformConfigBuffer.Bind();
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(config), config, GL_STREAM_DRAW);

	//Indexing
	wdata->m_waveformIndexBuffer.Bind();
	glBufferData(GL_SHADER_STORAGE_BUFFER, indexBuffer.size()*sizeof(uint32_t), &indexBuffer[0], GL_STREAM_DRAW);

	dt = GetTime() - start;
	m_downloadTime += dt;

	wdata->m_geometryOK = true;
}

void WaveformArea::ResetTextureFiltering()
{
	//No texture filtering
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

bool WaveformArea::on_render(const Glib::RefPtr<Gdk::GLContext>& /*context*/)
{
	LogIndenter li;

	double start = GetTime();
	double dt = start - m_lastFrameStart;
	if(m_lastFrameStart > 0)
	{
		//LogDebug("Inter-frame time: %.3f ms (%.2f FPS)\n", dt*1000, 1/dt);
		m_frameTime += dt;
		m_frameCount ++;
	}
	m_lastFrameStart = start;

	//Everything we draw is 2D painter's algorithm.
	//Turn off some stuff we don't need, but leave blending on.
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	//On the first frame, figure out what the actual screen surface FBO is.
	if(m_firstFrame)
	{
		m_windowFramebuffer.InitializeFromCurrentFramebuffer();
		m_firstFrame = false;
	}

	//Pull vertical size from the scope early on no matter how we're rendering
	m_pixelsPerVolt = m_height / m_channel->GetVoltageRange();

	//TODO: Do persistence processing

	/*
	if(!m_persistence || m_persistenceClear)
	{
		m_persistenceClear = false;
	}
	else
		RenderPersistenceOverlay();
	*/

	//Download the main waveform to the GPU and kick off the compute shader for rendering it
	if(IsAnalog())
	{
		PrepareGeometry(m_waveformRenderData);
		RenderTrace(m_waveformRenderData);
	}

	//Launch software rendering passes and push the resulting data to the GPU
	ComputeAndDownloadCairoUnderlays();
	ComputeAndDownloadCairoOverlays();

	//Do compute shader rendering for digital waveforms
	for(auto overlay : m_overlays)
	{
		if(overlay->GetType() != OscilloscopeChannel::CHANNEL_TYPE_DIGITAL)
			continue;

		//Create render data if needed
		//(can't do this when m_waveformRenderData is created because decoders are added later on)
		if(m_overlayRenderData.find(overlay) == m_overlayRenderData.end())
			m_overlayRenderData[overlay] = new WaveformRenderData(overlay);

		//Create the texture
		auto wdat = m_overlayRenderData[overlay];
		wdat->m_waveformTexture.Bind();
		wdat->m_waveformTexture.SetData(m_width, m_height, NULL, GL_RGBA, GL_UNSIGNED_BYTE, GL_RGBA32F);
		ResetTextureFiltering();

		PrepareGeometry(wdat);
		RenderTrace(wdat);
	}

	//Make sure all compute shaders are done before we composite
	m_waveformComputeProgram.MemoryBarrier();

	//Final compositing of data being drawn to the screen
	m_windowFramebuffer.Bind(GL_FRAMEBUFFER);
	RenderCairoUnderlays();
	RenderMainTrace();
	RenderOverlayTraces();
	RenderCairoOverlays();

	//Sanity check
	GLint err = glGetError();
	if(err != 0)
		LogNotice("Render: err = %x\n", err);

	dt = GetTime() - start;
	m_renderTime += dt;

	return true;
}

void WaveformArea::RenderMainTrace()
{
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, m_plotRight, m_height);
	if(IsEye())
		RenderEye();
	else if(IsWaterfall())
		RenderWaterfall();
	else
		RenderTraceColorCorrection(m_waveformRenderData);
	glDisable(GL_SCISSOR_TEST);
}

void WaveformArea::RenderOverlayTraces()
{
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 0, m_plotRight, m_height);

	for(auto it : m_overlayRenderData)
		RenderTraceColorCorrection(it.second);

	glDisable(GL_SCISSOR_TEST);
}

void WaveformArea::RenderEye()
{
	auto peye = dynamic_cast<EyeDecoder2*>(m_channel);
	auto pcap = dynamic_cast<EyeCapture2*>(m_channel->GetData());
	if(peye == NULL)
		return;
	if(pcap == NULL)
		return;

	//It's an eye pattern! Just copy it directly into the waveform texture.
	m_eyeTexture.Bind();
	ResetTextureFiltering();
	m_eyeTexture.SetData(
		peye->GetWidth(),
		peye->GetHeight(),
		pcap->GetData(),
		GL_RED,
		GL_FLOAT,
		GL_RGBA32F);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	m_eyeProgram.Bind();
	m_eyeVAO.Bind();
	m_eyeProgram.SetUniform(m_eyeTexture, "fbtex", 0);
	m_eyeProgram.SetUniform(m_eyeColorRamp[m_parent->GetEyeColor()], "ramp", 1);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void WaveformArea::RenderWaterfall()
{
	auto pfall = dynamic_cast<WaterfallDecoder*>(m_channel);
	auto pcap = dynamic_cast<WaterfallCapture*>(m_channel->GetData());
	if(pfall == NULL)
		return;
	if(pcap == NULL)
		return;

	//Make sure timebase is correct
	pfall->SetTimeScale(m_group->m_pixelsPerXUnit);
	pfall->SetTimeOffset(m_group->m_xAxisOffset);

	//Just copy it directly into the waveform texture.
	m_eyeTexture.Bind();
	ResetTextureFiltering();
	m_eyeTexture.SetData(
		pfall->GetWidth(),
		pfall->GetHeight(),
		pcap->GetData(),
		GL_RED,
		GL_FLOAT,
		GL_RGBA32F);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	m_eyeProgram.Bind();
	m_eyeVAO.Bind();
	m_eyeProgram.SetUniform(m_eyeTexture, "fbtex", 0);
	m_eyeProgram.SetUniform(m_eyeColorRamp[m_parent->GetEyeColor()], "ramp", 1);

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void WaveformArea::RenderPersistenceOverlay()
{
	/*
	m_waveformFramebuffer.Bind(GL_FRAMEBUFFER);

	//Configure blending
	glEnable(GL_BLEND);
	glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
	glBlendColor(0, 0, 0, 0.01);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	//Draw a black overlay with a little bit of alpha (to make old traces decay)
	m_persistProgram.Bind();
	m_persistVAO.Bind();
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	*/
}

void WaveformArea::RenderTrace(WaveformRenderData* data)
{
	if(!data->m_geometryOK)
		return;

	//Round thread block size up to next multiple of the local size (must be power of two)
	int localSize = 2;
	int numCols = m_plotRight;
	if(0 != (numCols % localSize) )
	{
		numCols |= (localSize-1);
		numCols ++;
	}
	int numGroups = numCols / localSize;

	m_waveformComputeProgram.Bind();
	m_waveformComputeProgram.SetImageUniform(data->m_waveformTexture, "outputTex");
	data->m_waveformStorageBuffer.BindBase(1);
	data->m_waveformConfigBuffer.BindBase(2);
	data->m_waveformIndexBuffer.BindBase(3);
	m_waveformComputeProgram.DispatchCompute(numGroups, 1, 1);
}

void WaveformArea::RenderTraceColorCorrection(WaveformRenderData* data)
{
	if(!data->m_geometryOK)
		return;

	//Prepare to render
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
	m_colormapProgram.Bind();
	m_colormapVAO.Bind();

	//Draw the offscreen buffer to the onscreen buffer
	//as a textured quad. Apply color correction as we do this.
	Gdk::Color color(data->m_channel->m_displaycolor);
	m_colormapProgram.SetUniform(data->m_waveformTexture, "fbtex");
	m_colormapProgram.SetUniform(color.get_red_p(), "r");
	m_colormapProgram.SetUniform(color.get_green_p(), "g");
	m_colormapProgram.SetUniform(color.get_blue_p(), "b");

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void WaveformArea::ComputeAndDownloadCairoUnderlays()
{
	double tstart = GetTime();

	//Create the Cairo surface we're drawing on
	Cairo::RefPtr< Cairo::ImageSurface > surface =
		Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, m_width, m_height);
	Cairo::RefPtr< Cairo::Context > cr = Cairo::Context::create(surface);

	//Set up transformation to match GL's bottom-left origin
	cr->translate(0, m_height);
	cr->scale(1, -1);

	//Clear to a blank background
	cr->set_source_rgba(0, 0, 0, 1);
	cr->rectangle(0, 0, m_width, m_height);
	cr->fill();

	//Software rendering
	DoRenderCairoUnderlays(cr);

	m_cairoTime += (GetTime() - tstart);
	tstart = GetTime();

	//Update the texture
	//Tell GL it's RGBA even though it's BGRA, faster to invert in the shader than when downloading
	m_cairoTexture.Bind();
	ResetTextureFiltering();
	m_cairoTexture.SetData(
		m_width,
		m_height,
		surface->get_data());

	m_texDownloadTime += (GetTime() - tstart);
}

void WaveformArea::RenderCairoUnderlays()
{
	double tstart = GetTime();

	glDisable(GL_BLEND);

	//Draw the actual image
	m_cairoProgram.Bind();
	m_cairoVAO.Bind();
	m_cairoProgram.SetUniform(m_cairoTexture, "fbtex");
	m_cairoTexture.Bind();
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	m_compositeTime += (GetTime() - tstart);
}

void WaveformArea::ComputeAndDownloadCairoOverlays()
{
	double tstart = GetTime();

	//Create the Cairo surface we're drawing on
	Cairo::RefPtr< Cairo::ImageSurface > surface =
		Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32, m_width, m_height);
	Cairo::RefPtr< Cairo::Context > cr = Cairo::Context::create(surface);

	//Set up transformation to match GL's bottom-left origin
	cr->translate(0, m_height);
	cr->scale(1, -1);

	//Clear to a blank background
	cr->set_source_rgba(0, 0, 0, 0);
	cr->rectangle(0, 0, m_width, m_height);
	cr->set_operator(Cairo::OPERATOR_SOURCE);
	cr->fill();
	cr->set_operator(Cairo::OPERATOR_OVER);

	DoRenderCairoOverlays(cr);

	m_cairoTime += GetTime() - tstart;
	tstart = GetTime();

	//Get the image data and make a texture from it
	//Tell GL it's RGBA even though it's BGRA, faster to invert in the shader than when downloading
	m_cairoTextureOver.Bind();
	ResetTextureFiltering();
	m_cairoTextureOver.SetData(
		m_width,
		m_height,
		surface->get_data());

	m_texDownloadTime += GetTime() - tstart;
}

void WaveformArea::RenderCairoOverlays()
{
	double tstart = GetTime();

	//Configure blending for Cairo's premultiplied alpha
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);

	//Draw the actual image
	m_windowFramebuffer.Bind(GL_FRAMEBUFFER);
	m_cairoTextureOver.Bind();
	m_cairoProgram.Bind();
	m_cairoVAO.Bind();
	m_cairoProgram.SetUniform(m_cairoTextureOver, "fbtex");
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	m_compositeTime += GetTime() - tstart;
}

int64_t WaveformArea::XPositionToXAxisUnits(float pix)
{
	return m_group->m_xAxisOffset + PixelsToXAxisUnits(pix);
}

int64_t WaveformArea::PixelsToXAxisUnits(float pix)
{
	return pix / m_group->m_pixelsPerXUnit;
}

float WaveformArea::XAxisUnitsToPixels(int64_t t)
{
	return t * m_group->m_pixelsPerXUnit;
}

float WaveformArea::XAxisUnitsToXPosition(int64_t t)
{
	return XAxisUnitsToPixels(t - m_group->m_xAxisOffset);
}

float WaveformArea::PixelsToVolts(float pix)
{
	return pix / m_pixelsPerVolt;
}

float WaveformArea::VoltsToPixels(float volt)
{
	return volt * m_pixelsPerVolt;
}

float WaveformArea::VoltsToYPosition(float volt)
{
	return m_height/2 - VoltsToPixels(volt + m_channel->GetOffset());
}

float WaveformArea::DbToYPosition(float db)
{
	float plotheight = m_height - 2*m_padding;
	return m_padding - (db/70 * plotheight);
}

float WaveformArea::YPositionToVolts(float y)
{
	return PixelsToVolts(-1 * (y - m_height/2) ) - m_channel->GetOffset();
}
