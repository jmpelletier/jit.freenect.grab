/*
 Copyright 2010, Jean-Marc Pelletier
 jmp@jmpelletier.com
 
 This file is part of jit.freenect.grab.
 
 jit.freenect.grab is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 jit.freenect.grab is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with jit.freenect.grab.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "jit.common.h"
#include <libusb.h>
#include "libfreenect.h"

typedef float float4[4];
typedef char char16[16];

#define DEPTH_WIDTH 640
#define DEPTH_HEIGHT 480
#define RGB_WIDTH 640
#define RGB_HEIGHT 480
#define MAX_DEVICES 8

#define set_float4(a,b) {int vector_iterator;for(vector_iterator=0;vector_iterator<4;vector_iterator++)a[vector_iterator]= b;}
#define set_char16(a,b) {int vector_iterator;for(vector_iterator=0;vector_iterator<16;vector_iterator++)a[vector_iterator]= b;}

#define copy_float4(a,b) {int vector_iterator;for(vector_iterator=0;vector_iterator<4;vector_iterator++)b[vector_iterator]= a[vector_iterator];}
#define copy_char16(a,b) {int vector_iterator;for(vector_iterator=0;vector_iterator<16;vector_iterator++)b[vector_iterator]= a[vector_iterator];}

#define mult_scalar_float4(a,b,c) {int vector_iterator;for(vector_iterator=0;vector_iterator<4;vector_iterator++)c[vector_iterator]= a[vector_iterator] * b;}

typedef struct _jit_freenect_grab
{
	t_object		ob;
	freenect_device *device;
	uint32_t timestamp;
} t_jit_freenect_grab;

typedef struct _grab_data
{
	freenect_pixel *rgb_data;
	freenect_depth *depth_data;
	freenect_device *device;
	uint32_t timestamp;
} t_grab_data;

void *_jit_freenect_grab_class;

t_jit_err 				jit_freenect_grab_init(void);
t_jit_freenect_grab		*jit_freenect_grab_new(void);
void 					jit_freenect_grab_free(t_jit_freenect_grab *x);
t_jit_err 				jit_freenect_grab_matrix_calc(t_jit_freenect_grab *x, void *inputs, void *outputs);
void					jit_freenect_open(t_jit_freenect_grab *x, t_symbol *s, long argc, t_atom *argv);
void					jit_freenect_close(t_jit_freenect_grab *x, t_symbol *s, long argc, t_atom *argv);
void					rgb_callback(freenect_device *dev, freenect_pixel *pixels, uint32_t timestamp);
void					depth_callback(freenect_device *dev, freenect_depth *pixels, uint32_t timestamp);
void					copy_depth_data(freenect_depth *source, char *out_bp, t_jit_matrix_info *dest_info);
void					copy_rgb_data(freenect_pixel *source, char *out_bp, t_jit_matrix_info *dest_info);

freenect_context *context = NULL;
t_grab_data device_data[MAX_DEVICES];
int device_count = 0;

t_jit_err jit_freenect_grab_init(void)
{
	//long attrflags=0;
	int i;
	//t_jit_object *attr;
	t_jit_object *mop,*output;
	t_atom a[1];
	
	_jit_freenect_grab_class = jit_class_new("jit_freenect_grab",(method)jit_freenect_grab_new,(method)jit_freenect_grab_free, sizeof(t_jit_freenect_grab),0L);
  	
	//add mop
	mop = (t_jit_object *)jit_object_new(_jit_sym_jit_mop,0,2); //0inputs, 2 outputs
	
	//Prepare depth image, all values are hard-coded, may need to bequeried for safety?
	output = jit_object_method(mop,_jit_sym_getoutput,1);
	
	jit_atom_setsym(a,_jit_sym_float32); //default
	jit_object_method(output,_jit_sym_types,1,a);
	
	jit_attr_setlong(output,_jit_sym_minplanecount,1);
	jit_attr_setlong(output,_jit_sym_maxplanecount,1);
	
	//Prepare RGB image
	output = jit_object_method(mop,_jit_sym_getoutput,2);
	
	jit_atom_setsym(a,_jit_sym_char); //default
	jit_object_method(output,_jit_sym_types,1,a);
	
	jit_attr_setlong(output,_jit_sym_minplanecount,4);
	jit_attr_setlong(output,_jit_sym_maxplanecount,4);
	
	jit_class_addadornment(_jit_freenect_grab_class,mop);
	
	//add methods
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_open, "open", A_GIMME, 0L);
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_close, "close", A_GIMME, 0L);
	
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_grab_matrix_calc, "matrix_calc", A_CANT, 0L);
	
	//add attributes	
	
	//TODO -
	
	jit_class_register(_jit_freenect_grab_class);
	
	for(i=0;i<MAX_DEVICES;i++){
		device_data[i].device = NULL;
		device_data[i].rgb_data = NULL;
		device_data[i].depth_data = NULL;
	}
	
	return JIT_ERR_NONE;
}

t_jit_freenect_grab *jit_freenect_grab_new(void)
{
	t_jit_freenect_grab *x;
	
	if (x=(t_jit_freenect_grab *)jit_object_alloc(_jit_freenect_grab_class))
	{
		x->device = NULL;
		x->timestamp = 0;
	} else {
		x = NULL;
	}	
	return x;
}

void jit_freenect_grab_free(t_jit_freenect_grab *x)
{
	if(x->device){
		libusb_release_interface((libusb_device_handle *)x->device, 0);
	}
}

void jit_freenect_open(t_jit_freenect_grab *x,  t_symbol *s, long argc, t_atom *argv)
{
	int i;
	if(x->device){ //Already initialized
		post("A device is already open.");
		return;
	}
	
	if(device_count == (MAX_DEVICES - 1)){
		post("Reached maximum number of simultaneous devices.");
		return;
	}
	
	if(!context){
		if(freenect_init(&context, NULL) < 0){
			error("Failed to initialize context.");
			return;
		}
	}
	
	if(freenect_open_device(context, &x->device, 0) < 0){
		error("Could not open device.");
		x->device = NULL;
		return;
	}
	
	device_count++;
	
	freenect_set_depth_callback(x->device, depth_callback);
	freenect_set_rgb_callback(x->device, rgb_callback);
	freenect_set_rgb_format(x->device, FREENECT_FORMAT_RGB);
	
	for(i=0;i<MAX_DEVICES;i++){
		if(device_data[i].device == NULL){
			device_data[i].device = x->device;
			break;
		}
	}
}

void jit_freenect_close(t_jit_freenect_grab *x,  t_symbol *s, long argc, t_atom *argv)
{
	int i;
	//Device close not yet implemented in libfreenect - jmp 2010/11/17
	if(x->device){
		libusb_release_interface((libusb_device_handle *)x->device, 0);
		
		for(i=0;i<MAX_DEVICES;i++){
			if(device_data[i].device == x->device){
				device_data[i].device = NULL;
				break;
			}
		}
		
		x->device = NULL;
	}
}

t_jit_err jit_freenect_grab_matrix_calc(t_jit_freenect_grab *x, void *inputs, void *outputs)
{
	t_jit_err err=JIT_ERR_NONE;
	long depth_savelock=0,rgb_savelock=0;
	t_jit_matrix_info depth_minfo,rgb_minfo;
	void *depth_matrix,*rgb_matrix;
	char *depth_bp, *rgb_bp;
	int i;
	
	depth_matrix = jit_object_method(outputs,_jit_sym_getindex,0);
	rgb_matrix = jit_object_method(outputs,_jit_sym_getindex,1);
	
	if (x && depth_matrix && rgb_matrix) {
		
		depth_savelock = (long) jit_object_method(depth_matrix,_jit_sym_lock,1);
		rgb_savelock = (long) jit_object_method(rgb_matrix,_jit_sym_lock,1);
		
		jit_object_method(depth_matrix,_jit_sym_getinfo,&depth_minfo);
		jit_object_method(rgb_matrix,_jit_sym_getinfo,&rgb_minfo);
		
		if ((depth_minfo.type != _jit_sym_float32) || (rgb_minfo.type != _jit_sym_char)) //overkill, but you can never be too sure
		{
			err=JIT_ERR_MISMATCH_TYPE;
			goto out;
		}
		
		if ((depth_minfo.planecount != 1) || (rgb_minfo.planecount != 4)) //overkill, but you can never be too sure
		{
			err=JIT_ERR_MISMATCH_PLANE;
			goto out;
		}
		
		if ((depth_minfo.dim[0] != DEPTH_WIDTH) || (depth_minfo.dim[1] != DEPTH_HEIGHT) ||
			(rgb_minfo.dim[0] != RGB_WIDTH) || (rgb_minfo.dim[1] != RGB_HEIGHT))
		{
			depth_minfo.dimcount = 2;
			depth_minfo.dim[0] = DEPTH_WIDTH;
			depth_minfo.dim[1] = DEPTH_HEIGHT;
			rgb_minfo.dimcount = 2;
			rgb_minfo.dim[0] = RGB_WIDTH;
			rgb_minfo.dim[1] = RGB_HEIGHT;
			
			jit_object_method(depth_matrix,_jit_sym_setinfo,&depth_minfo);
			jit_object_method(rgb_matrix,_jit_sym_setinfo,&rgb_minfo);
			
			jit_object_method(depth_matrix,_jit_sym_getinfo,&depth_minfo);
			jit_object_method(rgb_matrix,_jit_sym_getinfo,&rgb_minfo);
			
		}
		
		if ((depth_minfo.dimcount != 2) || (rgb_minfo.dimcount != 2)) //overkill, but you can never be too sure
		{
			err=JIT_ERR_MISMATCH_DIM;
			goto out;
		}
		
		if(!x->device){
			//err=JIT_ERR_GENERIC;
			//error("No device currently open!");
			goto out;
		}
		
		jit_object_method(depth_matrix,_jit_sym_getdata,&depth_bp);
		if (!depth_bp) { err=JIT_ERR_INVALID_OUTPUT; goto out;}
		
		jit_object_method(rgb_matrix,_jit_sym_getdata,&rgb_bp);
		if (!rgb_bp) { err=JIT_ERR_INVALID_OUTPUT; goto out;}
		
		//Grab and copy matrices
		for(i=0;i<MAX_DEVICES;i++){
			if(device_data[i].device == x->device){
				if(device_data[i].timestamp != x->timestamp){
					if(device_data[i].rgb_data && device_data[i].depth_data){
						x->timestamp = device_data[i].timestamp;
						copy_depth_data(device_data[i].depth_data, depth_bp, &depth_minfo);
						copy_rgb_data(device_data[i].rgb_data, rgb_bp, &rgb_minfo);
					}
				}
			}
		}
		
	} else {
		return JIT_ERR_INVALID_PTR;
	}
	
out:
	jit_object_method(depth_matrix,gensym("lock"),depth_savelock);
	jit_object_method(rgb_matrix,gensym("lock"),rgb_savelock);
	return err;
}

void copy_depth_data(freenect_depth *source, char *out_bp, t_jit_matrix_info *dest_info)
{
	int i,j;
	float4 vec_a, vec_b;
	
	float *out;
	freenect_depth *in;
	
	if(!source || !out_bp || !dest_info){
		error("Invalid pointer in copy_depth_data.");
		return;
	}
	
	in = source;
	
	for(i=0;i<DEPTH_HEIGHT;i++){
		out = (float *)(out_bp + dest_info->dimstride[1] * i);
		for(j=0;j<DEPTH_WIDTH;j+=4){
			vec_a[0] = (float)in[0];
			vec_a[1] = (float)in[1];
			vec_a[2] = (float)in[2];
			vec_a[3] = (float)in[3];
			
			//The macros below are to ensure that the multiplication is vectorized (SSE)
			mult_scalar_float4(vec_a, (1.f / (float)0x7FF), vec_b); //Kinect depth is 11 bits in a 16-bit short, normalize to 0-1
			copy_float4(out, vec_b);
			
			out += 4;
			in += 4;
		}
	}
}

void copy_rgb_data(freenect_pixel *source, char *out_bp, t_jit_matrix_info *dest_info)
{
	int i,j;
	
	char *out;
	freenect_pixel *in;
	
	if(!source || !out_bp || !dest_info){
		error("Invalid pointer in copy_depth_data.");
		return;
	}
	
	in = source;
	
	for(i=0;i<RGB_HEIGHT;i++){
		out = out_bp + dest_info->dimstride[1] * i;
		for(j=0;j<RGB_WIDTH;j++){
			out[0] = 0xFF;
			out[1] = in[0];
			out[2] = in[1];
			out[3] = in[2];
			
			out += 4;
			in += 3;
		}
	}
}

void rgb_callback(freenect_device *dev, freenect_pixel *pixels, uint32_t timestamp){
	int i;
	for(i=0;i<MAX_DEVICES;i++){
		if(device_data[i].device == dev){
			device_data[i].rgb_data = pixels;
			device_data[i].timestamp = timestamp;
			break;
		}
	}
}

void depth_callback(freenect_device *dev, freenect_depth *pixels, uint32_t timestamp){
	int i;
	for(i=0;i<MAX_DEVICES;i++){
		if(device_data[i].device == dev){
			device_data[i].depth_data = pixels;
			device_data[i].timestamp = timestamp;
			break;
		}
	}
}
