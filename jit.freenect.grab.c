/*
 Copyright 2010, Jean-Marc Pelletier, Nenad Popov and Andrew Roth 
 jmp@jmpelletier.com
 
 This file is part of jit.freenect.grab.
  
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at
 
 http://www.apache.org/licenses/LICENSE-2.0
 
 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
 
 */

#include <pthread.h>
#include "jit.common.h"
#include <libusb.h>
#include "libfreenect.h"
#include <time.h>
//#include "usb_libusb10.h"

typedef float float4[4];
typedef char char16[16];

#define DEPTH_WIDTH 640
#define DEPTH_HEIGHT 480
#define RGB_WIDTH 640
#define RGB_HEIGHT 480
#define MAX_DEVICES 8

#define set_float4(a,b) {int vector_iterator;for(vector_iterator=0;vector_iterator<4;vector_iterator++)a[vector_iterator]= b;}
#define set_char16(a,b) {int vector_iterator;for(vector_iterator=0;vector_iterator<16;vector_iterator++)a[vector_iterator]= b;}

#define copy_float4(a,b) {int vector_iterator;for(vector_iterator=0;vector_iterator<4;vector_iterator++)b[vector_iterator] = a[vector_iterator];}
#define copy_char16(a,b) {int vector_iterator;for(vector_iterator=0;vector_iterator<16;vector_iterator++)b[vector_iterator] = a[vector_iterator];}

#define mult_scalar_float4(a,b,c) {int vector_iterator;for(vector_iterator=0;vector_iterator<4;vector_iterator++)c[vector_iterator]= a[vector_iterator] * b;}
#define add_scalar_float4(a,b,c) {int vector_iterator; for(vector_iterator=0;vector_iterator<4;vector_iterator++)c[vector_iterator] = a[vector_iterator] + b;}
#define div_scalar_float4_inv(a,b,c) {int vector_iterator; for(vector_iterator=0;vector_iterator<4;vector_iterator++)c[vector_iterator] = b / a[vector_iterator];}
#define sub_float4(a,b,c) {int vector_iterator; for(vector_iterator=0;vector_iterator<4;vector_iterator++)c[vector_iterator] = a[vector_iterator] - b[vector_iterator];}

typedef struct _jit_freenect_grab
{
	t_object		ob;
	char unique;
	char mode;
	char has_frames;
	long index;
	freenect_device *device;
	uint32_t timestamp;
} t_jit_freenect_grab;

typedef struct _grab_data
{
	freenect_pixel *rgb_data;
	freenect_depth *depth_data;
	freenect_device *device;
	uint32_t index;
	uint32_t rgb_timestamp;
	uint32_t depth_timestamp;
	char have_frames;
} t_grab_data;

enum thread_mess_type{
	NONE,
	OPEN,
	CLOSE,
	TERMINATE
};

typedef struct _thread_mess{
	enum thread_mess_type type;
	void *data;
} t_thread_mess;

void *_jit_freenect_grab_class;

t_jit_err 				jit_freenect_grab_init(void);
t_jit_freenect_grab		*jit_freenect_grab_new(void);
void 					jit_freenect_grab_free(t_jit_freenect_grab *x);
t_jit_err 				jit_freenect_grab_matrix_calc(t_jit_freenect_grab *x, void *inputs, void *outputs);
void					jit_freenect_open(t_jit_freenect_grab *x, t_symbol *s, long argc, t_atom *argv);
void					jit_freenect_close(t_jit_freenect_grab *x, t_symbol *s, long argc, t_atom *argv);
void					rgb_callback(freenect_device *dev, freenect_pixel *pixels, uint32_t timestamp);
void					depth_callback(freenect_device *dev, void *pixels, uint32_t timestamp);
void					copy_depth_data(freenect_depth *source, char *out_bp, t_jit_matrix_info *dest_info, int mode);
void					copy_rgb_data(freenect_pixel *source, char *out_bp, t_jit_matrix_info *dest_info);


freenect_context *context = NULL;
t_grab_data device_data[MAX_DEVICES];  // <-- TODO: Fix multi-camera functionality - jmp 2010/11/21
int device_count = 0;

pthread_mutex_t mutex;
t_thread_mess message;
pthread_t capture_thread;

int object_count = 0;

void *capture_threadfunc(void *arg)
{
	int done = 0;
	int i;
	int device_count = 0;
	freenect_context *f_ctx = NULL;
	freenect_device *f_dev[MAX_DEVICES];
	
	for(i=0;i<MAX_DEVICES;i++){
		f_dev[i] = NULL;
	}
	
	if (freenect_init(&f_ctx, NULL) < 0) {
		printf("freenect_init() failed\n");
		goto out;
	}
		
	while(!done){
		if(device_count > 0){
			if(freenect_process_events(f_ctx) < 0){
				break;
			}
		}
		
		pthread_mutex_lock(&mutex);
		
		if(message.type == TERMINATE){
			done = 1;
			message.type = NONE;
		}
		else if(message.type == OPEN){
			t_jit_freenect_grab *x;
			
			if(device_count < MAX_DEVICES){
				if (freenect_open_device(f_ctx, &(f_dev[device_count]), 0) < 0) {  //<-- index does not appear to be implemented yet in libfreenect - jmp
					printf("Could not open device\n");
					goto out;
				}
				
				freenect_set_depth_callback(f_dev[device_count], depth_callback);
				freenect_set_rgb_callback(f_dev[device_count], rgb_callback);
				freenect_set_rgb_format(f_dev[device_count], FREENECT_FORMAT_RGB);
				
				freenect_start_depth(f_dev[device_count]);
				freenect_start_rgb(f_dev[device_count]);
				
				device_data[device_count].device = f_dev[device_count]; //<-- index does not appear to be implemented yet in libfreenect - jmp
				device_data[device_count].index = 0; //<-- index does not appear to be implemented yet in libfreenect - jmp
				
				device_count++;
				
				x = message.data;
				x->device = f_dev[device_count];
			}
			
			message.type = NONE;	
		}
		else if(message.type == CLOSE){
			t_jit_freenect_grab *x = message.data;
			
			for(i=0;i<MAX_DEVICES;i++){
				if(f_dev[i] == x->device){
					/* Not good...
					int j;
					freenect_stop_depth(f_dev[i]);
					freenect_stop_rgb(f_dev[i]);
					
					
					libusb_release_interface(&(f_dev[i]->usb_cam.dev), 0);
					free(f_dev[i]);
					
					device_count--;
					
					for(j=i;j<MAX_DEVICES-1;j++){
						f_dev[j] = f_dev[j+1];
					}
					f_dev[j] = NULL;
					 */
				}
			}
		}
		
		pthread_mutex_unlock(&mutex);
	}
	
out:
	post("Exiting grabber thread.\n");
	
	
	for(i=0;i<MAX_DEVICES;i++){
		if(f_dev[i]){
			freenect_stop_depth(f_dev[i]);
			freenect_stop_rgb(f_dev[i]);
			//freenect_close_device(&(f_dev[i]));
		}
	}
	//The following gives "dereferencing pointer to incomplete type" error -- investigate - jmp 2010/11/21
	/*
	if(f_dev){
		libusb_release_interface(&(f_dev->usb_cam.dev), 0);
		free(f_dev);
	}
	
	if(f_ctx){
		fnusb_shutdown(f_ctx->usb);
		free(f_ctx);
	}
	*/
	
	pthread_exit(NULL);
	return NULL;
}

t_jit_err jit_freenect_grab_init(void)
{
	long attrflags=0;
	int i;
	t_jit_object *attr;
	t_jit_object *mop,*output;
	t_atom a[2];
	
	_jit_freenect_grab_class = jit_class_new("jit_freenect_grab",(method)jit_freenect_grab_new,(method)jit_freenect_grab_free, sizeof(t_jit_freenect_grab),0L);
  	
	//add mop
	mop = (t_jit_object *)jit_object_new(_jit_sym_jit_mop,0,2); //0 inputs, 2 outputs
	
	//Prepare depth image, all values are hard-coded, may need to be queried for safety?
	output = jit_object_method(mop,_jit_sym_getoutput,1);
	
	jit_atom_setsym(a,_jit_sym_float32); //default
	jit_object_method(output,_jit_sym_types,1,a);
	
	jit_attr_setlong(output,_jit_sym_minplanecount,1);
	jit_attr_setlong(output,_jit_sym_maxplanecount,1);
	
	jit_atom_setlong(&a[0], DEPTH_WIDTH);
	jit_atom_setlong(&a[1], DEPTH_HEIGHT);
	
	jit_object_method(output, _jit_sym_mindim, 2, a);  //Two dimensions, sizes in atom array
	jit_object_method(output, _jit_sym_maxdim, 2, a);
		
	//Prepare RGB image
	output = jit_object_method(mop,_jit_sym_getoutput,2);
	
	jit_atom_setsym(a,_jit_sym_char); //default
	jit_object_method(output,_jit_sym_types,1,a);
	
	jit_attr_setlong(output,_jit_sym_minplanecount,4);
	jit_attr_setlong(output,_jit_sym_maxplanecount,4);
	
	jit_atom_setlong(&a[0], RGB_WIDTH);
	jit_atom_setlong(&a[1], RGB_HEIGHT);
	
	jit_object_method(output, _jit_sym_mindim, 2, a);
	jit_object_method(output, _jit_sym_maxdim, 2, a);
	
	jit_class_addadornment(_jit_freenect_grab_class,mop);
	
	//add methods
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_open, "open", A_GIMME, 0L);
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_close, "close", A_GIMME, 0L);
	
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_grab_matrix_calc, "matrix_calc", A_CANT, 0L);
	
	//add attributes	
	
	attrflags = JIT_ATTR_GET_DEFER_LOW | JIT_ATTR_SET_USURP_LOW;
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"unique",_jit_sym_char,attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,unique));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"mode",_jit_sym_char,attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,mode));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attrflags = JIT_ATTR_GET_DEFER_LOW | JIT_ATTR_SET_OPAQUE;
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"index",_jit_sym_long,attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,index));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attrflags = JIT_ATTR_GET_OPAQUE_USER | JIT_ATTR_SET_OPAQUE_USER;
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"has_frames",_jit_sym_char,attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,has_frames));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	jit_class_register(_jit_freenect_grab_class);
	
	for(i=0;i<MAX_DEVICES;i++){
		device_data[i].device = NULL;
		device_data[i].rgb_data = NULL;
		device_data[i].depth_data = NULL;
	}
	
	object_count = 0;
	
	message.type = NONE;
	
	if (pthread_create(&capture_thread, NULL, capture_threadfunc, NULL)) {
		error("Failed to create capture thread.\n");
		return JIT_ERR_GENERIC;
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
		x->unique = 0;
		x->mode = 0;
		x->has_frames = 0;
		
		if(object_count == 0){
			if(pthread_mutex_init(&mutex, NULL)){
				error("Could not initialize mutex.");
				return NULL;
			}
		}
		
		object_count++;
		post("Object count: %d", object_count);
	} else {
		x = NULL;
	}	
	return x;
}

void jit_freenect_grab_free(t_jit_freenect_grab *x)
{
	jit_freenect_close(x, NULL, 0, NULL);
	
	object_count--;
	
	if(object_count == 0){
		pthread_mutex_lock(&mutex);
		message.type = TERMINATE;
		pthread_mutex_unlock(&mutex);
	}
}

void jit_freenect_open(t_jit_freenect_grab *x,  t_symbol *s, long argc, t_atom *argv)
{
	if(!argc){
		x->index = 0;	
	}
	else{
		x->index = jit_atom_getlong(argv);
	}
	pthread_mutex_lock(&mutex);
	message.type = OPEN;
	message.data = x;
	pthread_mutex_unlock(&mutex);
	
}

void jit_freenect_close(t_jit_freenect_grab *x,  t_symbol *s, long argc, t_atom *argv)
{
	pthread_mutex_lock(&mutex);
	message.type = CLOSE;
	message.data = x;
	pthread_mutex_unlock(&mutex);
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
			(rgb_minfo.dim[0] != RGB_WIDTH) || (rgb_minfo.dim[1] != RGB_HEIGHT)) //overkill, but you can never be too sure
		{
			err=JIT_ERR_MISMATCH_DIM;
			goto out;
		}
		
		if ((depth_minfo.dimcount != 2) || (rgb_minfo.dimcount != 2)) //overkill, but you can never be too sure
		{
			err=JIT_ERR_MISMATCH_DIM;
			goto out;
		}
		
		jit_object_method(depth_matrix,_jit_sym_getdata,&depth_bp);
		if (!depth_bp) { err=JIT_ERR_INVALID_OUTPUT; goto out;}
		
		jit_object_method(rgb_matrix,_jit_sym_getdata,&rgb_bp);
		if (!rgb_bp) { err=JIT_ERR_INVALID_OUTPUT; goto out;}
		 
		//Grab and copy matrices
		x->has_frames = 0;  //Assume there are no new frames
		for(i=0;i<MAX_DEVICES;i++){
			if(device_data[i].device == x->device){
				if(device_data[0].have_frames > 1){ //2 or more new frames: depth and rgb
					pthread_mutex_lock(&mutex);
					device_data[0].have_frames = 0; //Reset the frame counter
					pthread_mutex_unlock(&mutex);
					
					x->timestamp = MAX(device_data[0].rgb_timestamp,device_data[0].depth_timestamp);
					x->has_frames = 1; //We have new frames to output in "unique" mode
					
					//TODO: it might be worth sending these to their own threads for performance - jmp
					copy_depth_data(device_data[0].depth_data, depth_bp, &depth_minfo, x->mode);
					copy_rgb_data(device_data[0].rgb_data, rgb_bp, &rgb_minfo);
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

void copy_depth_data(freenect_depth *source, char *out_bp, t_jit_matrix_info *dest_info, int mode)
{
	int i,j;
	float4 vec_a, vec_b;
	
	float *out;
	freenect_depth *in;
	
	if(!source){
		return;	
	}
	
	if(!out_bp || !dest_info){
		error("Invalid pointer in copy_depth_data.");
		return;
	}
	
	in = source;
	
	if(mode == 1){ //Normalize 0-1
		for(i=0;i<DEPTH_HEIGHT;i++){
			out = (float *)(out_bp + dest_info->dimstride[1] * i);
			for(j=0;j<DEPTH_WIDTH;j+=4){
				
				vec_a[0] = (float)in[0];
				vec_a[1] = (float)in[1];
				vec_a[2] = (float)in[2];
				vec_a[3] = (float)in[3];
				
				//The macros below are to ensure that the multiplication is vectorized (SSE)
				mult_scalar_float4(vec_a, (1.f / (float)0x7FF), vec_b); //Kinect depth is 11 bits in a 16-bit short, normalize to 0-1
				copy_float4(vec_b, out);
				
				out += 4;
				in += 4;
			}
		}
	}
	else if(mode == 2){ //Normalize 1-0
		float4 vec_c;
		vec_c[0] = vec_c[1] = vec_c[2] = vec_c[3] = 1.f;
		for(i=0;i<DEPTH_HEIGHT;i++){
			out = (float *)(out_bp + dest_info->dimstride[1] * i);
			for(j=0;j<DEPTH_WIDTH;j+=4){
				
				vec_a[0] = (float)in[0];
				vec_a[1] = (float)in[1];
				vec_a[2] = (float)in[2];
				vec_a[3] = (float)in[3];
				
				//The macros below are to ensure that the multiplication is vectorized (SSE)
				mult_scalar_float4(vec_a, (1.f / (float)0x7FF), vec_b); //Kinect depth is 11 bits in a 16-bit short, normalize to 0-1
				sub_float4(vec_c, vec_b, vec_a);
				copy_float4(vec_a, out);
				
				out += 4;
				in += 4;
			}
		}
	}
	else if(mode == 3){ //Distance
		for(i=0;i<DEPTH_HEIGHT;i++){
			out = (float *)(out_bp + dest_info->dimstride[1] * i);
			for(j=0;j<DEPTH_WIDTH;j+=4){
				
				vec_a[0] = (float)in[0];
				vec_a[1] = (float)in[1];
				vec_a[2] = (float)in[2];
				vec_a[3] = (float)in[3];
				
				//from https://github.com/OpenKinect/openkinect/wiki/Imaging-Information
				// distance = 100 / (3.33 + depth * -0.00307)
				mult_scalar_float4(vec_a, -0.00307f, vec_b);
				add_scalar_float4(vec_b, 3.33f, vec_a);
				div_scalar_float4_inv(vec_a, 100.f, vec_b);
				
				copy_float4(vec_b, out);
				
				out += 4;
				in += 4;
			}
		}
	}
	else{ //Raw
		for(i=0;i<DEPTH_HEIGHT;i++){
			out = (float *)(out_bp + dest_info->dimstride[1] * i);
			for(j=0;j<DEPTH_WIDTH;j+=4){
				
				out[0] = (float)in[0];
				out[1] = (float)in[1];
				out[2] = (float)in[2];
				out[3] = (float)in[3];
				
				out += 4;
				in += 4;
			}
		}
	}
	
}

void copy_rgb_data(freenect_pixel *source, char *out_bp, t_jit_matrix_info *dest_info)
{
	int i,j;
	
	char *out;
	freenect_pixel *in;
	
	if(!source){
		return;
	}
	
	if(!out_bp || !dest_info){
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
			pthread_mutex_lock(&mutex);
			device_data[i].rgb_data = pixels;
			device_data[i].rgb_timestamp = timestamp;
			device_data[i].have_frames++;
			pthread_mutex_unlock(&mutex);
			break;
		}
	}
}

void depth_callback(freenect_device *dev, void *pixels, uint32_t timestamp){
	int i;
	for(i=0;i<MAX_DEVICES;i++){
		if(device_data[i].device == dev){
			pthread_mutex_lock(&mutex);
			device_data[i].depth_data = pixels;
			device_data[i].depth_timestamp = timestamp;
			device_data[i].have_frames++;
			pthread_mutex_unlock(&mutex);
			break;
		}
	}
}
