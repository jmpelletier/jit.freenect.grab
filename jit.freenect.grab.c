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
#include "freenect_internal.h"
#include <time.h>

#define DEPTH_WIDTH 640
#define DEPTH_HEIGHT 480
#define RGB_WIDTH 640
#define RGB_HEIGHT 480
#define MAX_DEVICES 8
#define CLOUD_SIZE (DEPTH_WIDTH+2)*DEPTH_HEIGHT*2
#define CLOUD_BLOCK 640
#define DISTANCE_THRESH 10.f * 10.f

typedef float float4[4];
#define mult_scalar_float4(a,b,c) {int vector_iterator; for(vector_iterator=0;vector_iterator<4;vector_iterator++)c[vector_iterator] = a[vector_iterator] * b;}

typedef union _lookup_data{
	long *l_ptr;
	float *f_ptr;
	double *d_ptr;
}t_lookup;

enum thread_mess_type{
	NONE,
	OPEN,
	CLOSE,
	TERMINATE
};

typedef struct _point3D{
	float x;
	float y;
	float z;
	float tex_x;
	float tex_y;
	float nx;
	float ny;
	float nz;
	float r;
	float g;
	float b;
	float a;
} t_point3D;

typedef struct _cloud{
	t_point3D *points;
	uint32_t count;
	uint32_t size;
	t_point3D *last;
} t_cloud;

typedef struct _jit_freenect_grab
{
	t_object         ob;
	char             unique;
	char             mode;
	float            threshold;
	char             has_frames;
	long             index;
	long             ndevices;
	t_atom           format;
	freenect_device  *device;
	uint32_t         timestamp;
	t_lookup         lut;
	t_symbol         *lut_type;
	long             tilt;
	long             accelcount;
	double           mks_accel[3];
	uint8_t          *rgb_data, *rgb_swap;
	uint16_t         *depth_data, *depth_swap;
	uint32_t         rgb_timestamp;
	uint32_t         depth_timestamp;
	char             have_depth_frames;
	char             have_rgb_frames;
	char             clear_depth;
	t_cloud          cloud;
	t_symbol         *type;
	float            *rgb;
	freenect_raw_tilt_state *state;
    
    pthread_mutex_t  cb_mutex;
} t_jit_freenect_grab;

typedef struct _obj_list
{
	t_jit_freenect_grab **objects;
	uint32_t count;
} t_obj_list;


void *_jit_freenect_grab_class;

t_symbol *s_rgb, *s_RGB;
t_symbol *s_ir, *s_IR;

t_jit_err               jit_freenect_grab_init(void);
t_jit_freenect_grab     *jit_freenect_grab_new(void);
void                    jit_freenect_grab_free(t_jit_freenect_grab *x);

void                    jit_freenect_grab_open(t_jit_freenect_grab *x, t_symbol *s, long argc, t_atom *argv);
void                    jit_freenect_grab_close(t_jit_freenect_grab *x, t_symbol *s, long argc, t_atom *argv);

t_jit_err               jit_freenect_grab_get_ndevices(t_jit_freenect_grab *x, void *attr, long *ac, t_atom **av);
t_jit_err               jit_freenect_grab_get_accel(t_jit_freenect_grab *x, void *attr, long *ac, t_atom **av);
t_jit_err               jit_freenect_grab_get_tilt(t_jit_freenect_grab *x, void *attr, long *ac, t_atom **av);
void					jit_freenect_grab_set_tilt(t_jit_freenect_grab *x,  void *attr, long argc, t_atom *argv);
void					jit_freenect_grab_set_format(t_jit_freenect_grab *x,  void *attr, long argc, t_atom *argv);

t_jit_err               jit_freenect_grab_set_mode(t_jit_freenect_grab *x, void *attr, long ac, t_atom *av);

t_jit_err               jit_freenect_grab_matrix_calc(t_jit_freenect_grab *x, void *inputs, void *outputs);
void                    copy_depth_data(uint16_t *source, char *out_bp, t_jit_matrix_info *dest_info, t_lookup *lut);
//void                    build_geometry(t_jit_freenect_grab *x, void *matrix, char *out_bp, t_jit_matrix_info *dest_info);
void                    copy_rgb_data(uint8_t *source, char *out_bp, t_jit_matrix_info *dest_info);

void                    rgb_callback(freenect_device *dev, void *pixels, uint32_t timestamp);
void                    depth_callback(freenect_device *dev, void *pixels, uint32_t timestamp);

pthread_t capture_thread;
int       terminate_thread;

int object_count = 0;

freenect_context *f_ctx = NULL;

float xlut[640];
float ylut[480];

/*

static int grow_cloud(t_cloud *cloud){
	t_point3D *temp = (t_point3D *)realloc(cloud->points, (cloud->size + CLOUD_BLOCK)*sizeof(t_point3D));
	if(!temp){
		error("Out of memory, cannot grow cloud.");
		return 1;
	}
	cloud->points = temp;
	cloud->size += CLOUD_BLOCK;
	return 0;
}

static void push_cloud_point(t_cloud *cloud, float x, float y, float z, float tx, float ty, 
							 float nx, float ny, float nz, float r, float g, float b, float a){
	if(cloud->count == cloud->size){
		if(grow_cloud(cloud))return; //Out of memory
	}
	cloud->points[cloud->count].x = x;
	cloud->points[cloud->count].y = y;
	cloud->points[cloud->count].z = z;
	cloud->points[cloud->count].tex_x = tx;
	cloud->points[cloud->count].tex_y = ty;
	cloud->points[cloud->count].nx = nx;
	cloud->points[cloud->count].ny = ny;
	cloud->points[cloud->count].nz = nz;
	cloud->points[cloud->count].r = r;
	cloud->points[cloud->count].g = g;
	cloud->points[cloud->count].b = b;
	cloud->points[cloud->count].a = a;
	cloud->count++;
}

static void start_cloud_point(t_cloud *cloud, float x, float y, float z, float tx, float ty, 
							  float nx, float ny, float nz, float r, float g, float b, float a){
	if(cloud->count > (cloud->size - 2)){
		if(grow_cloud(cloud))return; //Out of memory
	}
	cloud->points[cloud->count].x = x;
	cloud->points[cloud->count].y = y;
	cloud->points[cloud->count].z = z;
	cloud->points[cloud->count].tex_x = tx;
	cloud->points[cloud->count].tex_y = ty;
	cloud->points[cloud->count].nx = nx;
	cloud->points[cloud->count].ny = ny;
	cloud->points[cloud->count].nz = nz;
	cloud->points[cloud->count].r = r;
	cloud->points[cloud->count].g = g;
	cloud->points[cloud->count].b = b;
	cloud->points[cloud->count].a = a;
	cloud->count++;
	cloud->points[cloud->count] = cloud->points[cloud->count-1];
	cloud->count++;
}

static void terminate_cloud_point(t_cloud *cloud){
	if(cloud->count == cloud->size){
		if(grow_cloud(cloud))return; //Out of memory
	}
	cloud->points[cloud->count] = cloud->points[cloud->count-1];
	cloud->count++;
}

static void release_cloud(t_cloud *cloud){
	free(cloud->points);
	cloud->points = NULL;
	cloud->count = 0;
}

static void allocate_cloud(t_cloud *cloud){
	if(!cloud->points){
		cloud->points = (t_point3D *)malloc(CLOUD_SIZE*sizeof(t_point3D));
		if(!cloud->points){
			cloud->size = 0;
			cloud->count = 0;
			error("Out of memory, could not allocate cloud.");
		}
		cloud->size = CLOUD_SIZE;
		cloud->count = 0; 
	}
}
 
*/

void calculate_lut(t_lookup *lut, t_symbol *type, int mode){
	long i;
	
	if(type == _jit_sym_float32){
		float *f_lut;
		f_lut = (float *)realloc(lut->f_ptr, sizeof(float) * 0x800);
		if(!f_lut){
			error("Out of memory!");
			return;
		}
		lut->f_ptr = f_lut;
		
		switch(mode){
			case 0:
				for(i=0;i<0x800;i++){
					lut->f_ptr[i] = (float)i;
				}
				break;
			case 1:
				for(i=0;i<0x800;i++){
					lut->f_ptr[i] = (float)i * (1.f / (float)0x7FF);
				}
				break;
			case 2:
				for(i=0;i<0x800;i++){
					lut->f_ptr[i] = 1.f - ((float)i * (1.f / (float)0x7FF));
				}
				break;
			case 3:
			case 4:
				for(i=0;i<0x800;i++){
					lut->f_ptr[i] = 10.f / (3.33f + (float)i * -0.00307f);
				} 
				break;
		}
	}
	else if(type == _jit_sym_long){
		long *l_lut;
		l_lut = (long *)realloc(lut->l_ptr, sizeof(long) * 0x800);
		if(!l_lut){
			error("Out of memory!");
			return;
		}
		lut->l_ptr = l_lut;
		
		switch(mode){
			case 0:
			case 1:
				for(i=0;i<0x800;i++){
					lut->l_ptr[i] = i;
				}
				break;
			case 2:
				for(i=0;i<0x800;i++){
					lut->l_ptr[i] = 0x7FF - i;
				}
				break;
			case 3:
				for(i=0;i<0x800;i++){
					lut->l_ptr[i] = (long)(-10.f / (3.33f + (float)i * -0.00307f));
				} 
				break;
			case 4:
				for(i=0;i<0x800;i++){
					lut->l_ptr[i] = (long)(-10.f / (3.33f + (float)i * -0.00307f));
				} 
				break;
		}
	}
	else if(type == _jit_sym_float64){
		double *d_lut;
		d_lut = (double *)realloc(lut->d_ptr, sizeof(double) * 0x800);
		if(!d_lut){
			error("Out of memory!");
			return;
		}
		lut->d_ptr = d_lut;
		
		switch(mode){
			case 0:
				for(i=0;i<0x800;i++){
					lut->d_ptr[i] = (double)i;
				}
				break;
			case 1:
				for(i=0;i<0x800;i++){
					lut->d_ptr[i] = (double)i * (1.0 / (double)0x7FF);
				}
				break;
			case 2:
				for(i=0;i<0x800;i++){
					lut->d_ptr[i] = 1.0 - ((double)i * (1.0 / (double)0x7FF));
				}
				break;
			case 3:
				for(i=0;i<0x800;i++){
					lut->d_ptr[i] = -10.0 / (3.33 + (double)i * -0.00307);
				} 
				break;
			case 4:
				for(i=0;i<0x800;i++){
					lut->d_ptr[i] = -10.0 / (3.33 + (double)i * -0.00307);
				} 
				break;
		}
	}
	else if(type == NULL){
		if(lut->f_ptr){
			free(lut->f_ptr);
			lut->f_ptr = NULL;
		}
	}
	else{
		error("Invalid type for lookup table calculation. char not supported.");
		return;
	}
}

void *capture_threadfunc(void *arg)
{	
	freenect_context *context;
		
	if(!f_ctx){
		if (freenect_init(&context, NULL) < 0) {
			printf("freenect_init() failed\n");
			goto out;
		}
	}
	
	f_ctx = context;
	
	terminate_thread = 0;
		
	while(!terminate_thread){
		if(f_ctx->first){
			if(freenect_process_events(f_ctx) < 0){
				error("Could not process events.");
				break;
			}
		}
		
		sleep(0);
	}
	
out:
	freenect_shutdown(f_ctx);
	f_ctx = NULL;
	pthread_exit(NULL);
	return NULL;
}

t_jit_err jit_freenect_grab_init(void)
{
	long attrflags=0;
	t_jit_object *attr;
	t_jit_object *mop,*output;
	t_atom a[4];
	int i;
	
	s_rgb = gensym("rgb");
	s_RGB = gensym("RGB");
	s_ir = gensym("ir");
	s_IR = gensym("IR");
	
	_jit_freenect_grab_class = jit_class_new("jit_freenect_grab",(method)jit_freenect_grab_new,
											 (method)jit_freenect_grab_free, sizeof(t_jit_freenect_grab),0L);
  	
	//add mop
	mop = (t_jit_object *)jit_object_new(_jit_sym_jit_mop,0,2); //0 inputs, 2 outputs
	
	//Prepare depth image, all values are hard-coded, may need to be queried for safety?
	output = jit_object_method(mop,_jit_sym_getoutput,1);
		
	jit_atom_setsym(a,_jit_sym_float32); //default
	jit_atom_setsym(a+1,_jit_sym_long);
	jit_atom_setsym(a+2,_jit_sym_float64);
	jit_object_method(output,_jit_sym_types,3,a);
	
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
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_grab_open, "open", A_GIMME, 0L);
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_grab_close, "close", A_GIMME, 0L);
	
	jit_class_addmethod(_jit_freenect_grab_class, (method)jit_freenect_grab_matrix_calc, "matrix_calc", A_CANT, 0L);
	
	//add attributes	
	attrflags = JIT_ATTR_GET_DEFER_LOW | JIT_ATTR_SET_USURP_LOW;
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"unique",_jit_sym_char,
										  attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,unique));
	jit_attr_addfilterset_clip(attr,0,1,TRUE,TRUE);
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"threshold",_jit_sym_float32,
										  attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,threshold));
	jit_attr_addfilterset_clip(attr,0,0,TRUE,FALSE);
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	//attrflags = JIT_ATTR_GET_DEFER_LOW | JIT_ATTR_SET_USURP_LOW;
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"cleardepth",_jit_sym_char,
										  attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,clear_depth));
	jit_attr_addfilterset_clip(attr,0,1,TRUE,TRUE);
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"mode",_jit_sym_char,
										  attrflags,(method)NULL,(method)jit_freenect_grab_set_mode,calcoffset(t_jit_freenect_grab,mode));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"format",_jit_sym_atom,
										  attrflags,(method)NULL,(method)jit_freenect_grab_set_format,calcoffset(t_jit_freenect_grab,format));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"tilt",_jit_sym_long,
										  attrflags,(method)jit_freenect_grab_get_tilt,(method)jit_freenect_grab_set_tilt,
										  calcoffset(t_jit_freenect_grab,tilt));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attrflags = JIT_ATTR_GET_DEFER_LOW | JIT_ATTR_SET_OPAQUE;
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"index",_jit_sym_long,
										  attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,index));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"ndevices",_jit_sym_long,
										  attrflags,(method)jit_freenect_grab_get_ndevices,(method)NULL,calcoffset(t_jit_freenect_grab,ndevices));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset_array, "accel", _jit_sym_float64, 3, 
										  attrflags, (method)jit_freenect_grab_get_accel,(method)NULL, 
										  calcoffset(t_jit_freenect_grab, accelcount),calcoffset(t_jit_freenect_grab,mks_accel));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	attrflags = JIT_ATTR_GET_OPAQUE_USER | JIT_ATTR_SET_OPAQUE_USER;
	attr = (t_jit_object *)jit_object_new(_jit_sym_jit_attr_offset,"has_frames",_jit_sym_char,
										  attrflags,(method)NULL,(method)NULL,calcoffset(t_jit_freenect_grab,has_frames));
	jit_class_addattr(_jit_freenect_grab_class,attr);
	
	jit_class_register(_jit_freenect_grab_class);
	
	//Prepare lut for OpenGL output
	
	for(i=0;i<640;i++){
		xlut[i] = 0.542955699638437f * (((float)i - 319.5f) / 319.5f);
	}
	
	for(i=0;i<480;i++){
		ylut[i] = -0.393910475614942f * (((float)i - 239.5f) / 239.5f);
	}
			
	return JIT_ERR_NONE;
}

t_jit_freenect_grab *jit_freenect_grab_new(void)
{
	t_jit_freenect_grab *x;
	
	if ((x=(t_jit_freenect_grab *)jit_object_alloc(_jit_freenect_grab_class)))
	{
		x->device = NULL;
		x->timestamp = 0;
		x->unique = 0;
		x->mode = 3;
		x->has_frames = 0;
		x->ndevices = 0;
		x->lut.f_ptr = NULL;
		x->lut_type = NULL;
		x->tilt = 0;
		x->state = NULL;
		x->clear_depth = 0;
		x->cloud.points = NULL;
		x->cloud.count = 0;
		x->cloud.size = 0;
		x->type = NULL;
		x->threshold = 2.f;
		x->rgb = NULL;
        
        pthread_mutex_init(&x->cb_mutex, NULL);
		jit_atom_setsym(&x->format, s_rgb);
		
	} else {
		x = NULL;
	}	
	return x;
}

void jit_freenect_grab_free(t_jit_freenect_grab *x)
{
	jit_freenect_grab_close(x, NULL, 0, NULL);
			
	if(x->lut.f_ptr){
		free(x->lut.f_ptr);
	}
	
	//release_cloud(&x->cloud);
}

t_jit_err jit_freenect_grab_get_ndevices(t_jit_freenect_grab *x, void *attr, long *ac, t_atom **av){
	
	if ((*ac)&&(*av)) {
		
	} else {
		*ac = 1;
		if (!(*av = jit_getbytes(sizeof(t_atom)*(*ac)))) {
			*ac = 0;
			return JIT_ERR_OUT_OF_MEM;
		}
	}
	
	if(f_ctx){
		x->ndevices = freenect_num_devices(f_ctx);
	}
	else{
		x->ndevices = 0;
	}
	
	
	jit_atom_setlong(*av,x->ndevices);
	
	return JIT_ERR_NONE;
}

void jit_freenect_grab_set_format(t_jit_freenect_grab *x,  void *attr, long argc, t_atom *argv){
	if(argc){
		t_atom a;
		if(argv->a_type == A_SYM){
			if((argv->a_w.w_sym == s_rgb)||((argv->a_w.w_sym == s_RGB))){
				jit_atom_setsym(&a, s_rgb);
			}
			else if((argv->a_w.w_sym == s_ir)||((argv->a_w.w_sym == s_IR))){
				jit_atom_setsym(&a, s_ir);
			}
			else{
				error("Invalid output format: %s", argv->a_w.w_sym->s_name);
				return;
			}
		}
		else{
			long v = jit_atom_getlong(argv);
			if(v <= 0){
				jit_atom_setsym(&a, s_rgb);
			}
			else{
				jit_atom_setsym(&a, s_ir);
			}
		}
		if(argv->a_w.w_sym == x->format.a_w.w_sym)return;
		
		x->format = a;
		
		if(x->device){
			/*
			if(x->format.a_w.w_sym == s_ir){
				freenect_set_video_format(x->device, FREENECT_VIDEO_IR_8BIT);
			}
			else{
				freenect_set_video_format(x->device, FREENECT_VIDEO_RGB);
			}
			*/
			/*
			t_atom arg;
			jit_atom_setlong(&arg, x->index);
			jit_freenect_grab_close(x, NULL, 0, NULL);
			jit_freenect_grab_open(x, NULL, 1, &arg);
			 */
			//jit_freenect_grab_close(x, NULL, 0, NULL);
			post("jit.freenect.grab: Cannot change output format while running. Please close and re-open device to activate change.");
		}
		
		
	}
}

t_jit_err jit_freenect_grab_get_accel(t_jit_freenect_grab *x, void *attr, long *ac, t_atom **av){
	double ax=0,ay=0,az=0;
	
	if ((*ac)&&(*av)) {
		
	} else {
		*ac = 3;
		if (!(*av = jit_getbytes(sizeof(t_atom)*(*ac)))) {
			*ac = 0;
			return JIT_ERR_OUT_OF_MEM;
		}
	}

	if(x->device){
		freenect_update_tilt_state(x->device);
		x->state = freenect_get_tilt_state(x->device);
		if (x->state)
			freenect_get_mks_accel(x->state, &ax, &ay, &az);
	}
	
	jit_atom_setfloat(*av, ax);
	jit_atom_setfloat(*av +1, ay);
	jit_atom_setfloat(*av +2, az);
		
	return JIT_ERR_NONE;
}

t_jit_err jit_freenect_grab_get_tilt(t_jit_freenect_grab *x, void *attr, long *ac, t_atom **av){
	double tilt=0;
	
	
	if ((*ac)&&(*av)) {
		
	} else {
		*ac = 1;
		if (!(*av = jit_getbytes(sizeof(t_atom)*(*ac)))) {
			*ac = 0;
			return JIT_ERR_OUT_OF_MEM;
		}
	}
	
	if(x->device){
		freenect_update_tilt_state(x->device);
		x->state = freenect_get_tilt_state(x->device);
		if (x->state)
			tilt = freenect_get_tilt_degs(x->state);
		
	}
	
	jit_atom_setfloat(*av, tilt);
	
	
	return JIT_ERR_NONE;
}

t_jit_err jit_freenect_grab_set_mode(t_jit_freenect_grab *x, void *attr, long ac, t_atom *av){
    if(ac < 1){
        return JIT_ERR_NONE;
    }
	
	if(x->mode != jit_atom_getlong(av)){
		long mode = jit_atom_getlong(av);
		
		CLIP(mode, 0, 3);  // <-- See explanation in build_geometry implementation as to why I'm blocking point cloud output for now - jmp 2011-01-11
		/*
		if(mode == 4){
			if(!x->cloud.points){
				allocate_cloud(&x->cloud);
			}
			if(!x->rgb){
				x->rgb = malloc(DEPTH_WIDTH*DEPTH_HEIGHT*3*sizeof(float));
				if(!x->rgb){
					return JIT_ERR_OUT_OF_MEM;
				}
			}
		}
		else{
			release_cloud(&x->cloud);
			if(x->rgb){
				free(x->rgb);
				x->rgb = NULL;
			}
		}
		*/
		
		calculate_lut(&x->lut, x->lut_type, mode);
		
		x->mode = mode;
	}
	
    return JIT_ERR_NONE;
}

void jit_freenect_grab_set_tilt(t_jit_freenect_grab *x,  void *attr, long argc, t_atom *argv)
{
	if(argv){
		x->tilt = jit_atom_getlong(argv);
		
		CLIP(x->tilt, -30, 30);
		
		if(x->device){
			freenect_set_tilt_degs(x->device,x->tilt);
		}
	}
}

void jit_freenect_grab_open(t_jit_freenect_grab *x,  t_symbol *s, long argc, t_atom *argv)
{
	int ndevices, devices_left, dev_ndx;
	t_jit_freenect_grab *y;
	freenect_device *dev;

	if(x->device){
		post("A device is already open.");
		return;
	}
	
	if(!f_ctx){
		if (pthread_create(&capture_thread, NULL, capture_threadfunc, NULL)) {
			error("Failed to create capture thread.");
			return;
		}
		while(!f_ctx){
			sleep(0);
		}
	}
	
	ndevices = freenect_num_devices(f_ctx);
	
	if(!ndevices){
		post("Could not find any connected Kinect device. Are you sure the power cord is plugged-in?");
		return;
	}
	
	devices_left = ndevices;
	dev = f_ctx->first;
	while(dev){
		dev = dev->next;
		devices_left--;
	}
	
	if(!devices_left){
		post("All Kinect devices are currently in use.");
		return;
	}
	
	if(!argc){
		x->index = 0;	
	}
	else{
		//Is the device already in use?
		x->index = jit_atom_getlong(argv);
		
		dev = f_ctx->first;
		while(dev){
			y = freenect_get_user(dev);
			if(y->index == x->index){
				post("Kinect device %d is already in use.", x->index);
				x->index = 0;
				return;
			}
			dev = dev->next;
		}
	}
	
	if(x->index > ndevices){
		post("Cannot open Kinect device %d, only %d are connected.", x->index, ndevices);
		x->index = 0;
		return;
	}
	
	//Find out which device to open
	dev_ndx = x->index;
	if(!dev_ndx){
		int found = 0;
		while(!found){
			found = 1;
			dev = f_ctx->first;
			while(dev){
				y = freenect_get_user(dev);
				if(y->index-1 == dev_ndx){
					found = 0;
					break;
				}
				dev = dev->next;
			}
			dev_ndx++;
		}
		x->index = dev_ndx;
	}
		
	if (freenect_open_device(f_ctx, &(x->device), dev_ndx-1) < 0) {
		error("Could not open Kinect device %d", dev_ndx);
		x->index = 0;
		x->device = NULL;
	}
	
	freenect_set_depth_callback(x->device, depth_callback);
	freenect_set_video_callback(x->device, rgb_callback);
	if(x->format.a_w.w_sym == s_ir){
		freenect_set_video_mode(x->device, freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_IR_8BIT));
	}
	else{
		freenect_set_video_mode(x->device, freenect_find_video_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_VIDEO_RGB));
	}
	
	
	freenect_set_depth_mode(x->device, freenect_find_depth_mode(FREENECT_RESOLUTION_MEDIUM, FREENECT_DEPTH_11BIT));
	//freenect_set_video_buffer(x->device, rgb_back);
	
	//Store a pointer to this object in the freenect device struct (for use in callbacks)
	freenect_set_user(x->device, x);  
	
	freenect_set_led(x->device,LED_RED);
	
	//freenect_set_tilt_degs(x->device,x->tilt);
	
	freenect_start_depth(x->device);
	freenect_start_video(x->device);
}

void jit_freenect_grab_close(t_jit_freenect_grab *x,  t_symbol *s, long argc, t_atom *argv)
{
	if(!x->device)return;
	freenect_set_led(x->device,LED_BLINK_GREEN);
	freenect_close_device(x->device);
	x->device = NULL;
	if(!f_ctx->first){
		terminate_thread = 1;
	}
}

t_jit_err jit_freenect_grab_matrix_calc(t_jit_freenect_grab *x, void *inputs, void *outputs)
{
	t_jit_err err=JIT_ERR_NONE;
	long depth_savelock=0,rgb_savelock=0;
	t_jit_matrix_info depth_minfo,rgb_minfo;
	void *depth_matrix,*rgb_matrix;
	char *depth_bp, *rgb_bp;
			
	depth_matrix = jit_object_method(outputs,_jit_sym_getindex,0);
	rgb_matrix = jit_object_method(outputs,_jit_sym_getindex,1); 
	
	if (x && depth_matrix && rgb_matrix) {
		
		depth_savelock = (long) jit_object_method(depth_matrix,_jit_sym_lock,1);
		rgb_savelock = (long) jit_object_method(rgb_matrix,_jit_sym_lock,1);
		
		if(!x->device){
			goto out;
		}
		
		jit_object_method(depth_matrix,_jit_sym_getinfo,&depth_minfo);
		jit_object_method(rgb_matrix,_jit_sym_getinfo,&rgb_minfo);
		
		if ((depth_minfo.type == _jit_sym_char) || (rgb_minfo.type != _jit_sym_char)) 
		{
			err=JIT_ERR_MISMATCH_TYPE;
			goto out;
		}
		
		if(x->device->video_format == FREENECT_VIDEO_IR_8BIT){
			if(rgb_minfo.planecount != 1){
				rgb_minfo.planecount = 1;
				jit_object_method(rgb_matrix, _jit_sym_setinfo, &rgb_minfo);
				jit_object_method(rgb_matrix, _jit_sym_getinfo, &rgb_minfo);
			}
		}
		else{
			if(rgb_minfo.planecount != 4){
				rgb_minfo.planecount = 4;
				jit_object_method(rgb_matrix, _jit_sym_setinfo, &rgb_minfo);
				jit_object_method(rgb_matrix, _jit_sym_getinfo, &rgb_minfo);
			}
		}
		
		/*
		if (rgb_minfo.planecount != 4) //overkill, but you can never be too sure
		{
			err=JIT_ERR_MISMATCH_PLANE;
			goto out;
		}
		 */
		
		if(x->type==NULL){
			x->type = depth_minfo.type;
		}
		
		if((depth_minfo.planecount != 1)&&(x->mode < 4)){
			depth_minfo.planecount = 1;
			depth_minfo.type = x->type;
			depth_minfo.dimcount = 2;
			depth_minfo.dim[0] = DEPTH_WIDTH;
			depth_minfo.dim[1] = DEPTH_HEIGHT;
			depth_minfo.flags = 0L;
			jit_object_method(depth_matrix,_jit_sym_setinfo_ex,&depth_minfo);
			jit_object_method(depth_matrix,_jit_sym_getinfo,&depth_minfo);
		}
		
		if((x->mode < 4)&&(x->type != depth_minfo.type)){
			x->type = depth_minfo.type;
		}
		
		jit_object_method(depth_matrix,_jit_sym_getdata,&depth_bp);
		if (!depth_bp) { err=JIT_ERR_INVALID_OUTPUT; goto out;}
		
		jit_object_method(rgb_matrix,_jit_sym_getdata,&rgb_bp);
		if (!rgb_bp) { err=JIT_ERR_INVALID_OUTPUT; goto out;}
		
		if((depth_minfo.type != x->lut_type) || !x->lut.f_ptr){
			calculate_lut(&x->lut, depth_minfo.type, x->mode);
			x->lut_type = depth_minfo.type;
		}
		 
		//Grab and copy matrices
		x->has_frames = 0;  //Assume there are no new frames
		
		if(x->have_rgb_frames || x->have_depth_frames){
			x->timestamp = MAX(x->rgb_timestamp,x->depth_timestamp);
			
			if(x->have_rgb_frames){
				copy_rgb_data(x->rgb_data, rgb_bp, &rgb_minfo);
				x->have_rgb_frames = 0;
			}
			
			if(x->have_depth_frames){
				//if(x->mode == 4){
				//	build_geometry(x, depth_matrix, depth_bp, &depth_minfo);
				//}
				//else{
					copy_depth_data(x->depth_data, depth_bp, &depth_minfo, &x->lut);
				//}
				x->have_depth_frames = 0;
				x->has_frames = 1;
			}
			else if((x->clear_depth)&&((x->rgb_timestamp - x->depth_timestamp)>3000000)){
				jit_object_method(depth_matrix, _jit_sym_clear);
				x->has_frames = 1;
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

void copy_depth_data(uint16_t *source, char *out_bp, t_jit_matrix_info *dest_info, t_lookup *lut)
{
	int i,j;
	uint16_t *in;
	
	
	if(!source){
		return;	
	}
	
	if(!out_bp || !dest_info){
		error("Invalid pointer in copy_depth_data.");
		return;
	}
	
	in = source;
	
	if(dest_info->type == _jit_sym_float32){
		float *out;
		for(i=0;i<DEPTH_HEIGHT;i++){
			out = (float *)(out_bp + dest_info->dimstride[1] * i);
			for(j=0;j<DEPTH_WIDTH;j++){
				out[j] = lut->f_ptr[*in];
				in++;
			}
		}
	}
	else if(dest_info->type == _jit_sym_float64){
		double *out;
		for(i=0;i<DEPTH_HEIGHT;i++){
			out = (double *)(out_bp + dest_info->dimstride[1] * i);
			for(j=0;j<DEPTH_WIDTH;j++){
				out[j] = lut->d_ptr[*in];
				in++;
			}
		}
	}
	else if(dest_info->type == _jit_sym_long){
		long *out;
		for(i=0;i<DEPTH_HEIGHT;i++){
			out = (long *)(out_bp + dest_info->dimstride[1] * i);
			for(j=0;j<DEPTH_WIDTH;j++){
				out[j] = lut->l_ptr[*in];
				in++;
			}
		}
	}
}

/*
 
 Note: The code below works but frame rate in Max is devilishly slow. Shark reports significant load from
 jit_matrix_frommatrix_2d_float32. Frame rate on the OpenGL side is also sluggish, more than it should be.
 It would be better to have a separate external write directly to OpenGL instead of bothering with this
 point cloud matrix. This is why, for the moment I'm leaving the code, but leaving it unaccessible, as it's
 near useless as it is. jmp - 2011-01-10
*/
/*
void build_geometry(t_jit_freenect_grab *x, void *matrix, char *out_bp, t_jit_matrix_info *dest_info){
	int i,i2,j;
	uint16_t *in, *in2;
	float d=0, d2, pd=0, dd;
	int valid = 0;
	uint16_t *source = x->depth_data;
	uint8_t *c, *rgb = x->rgb_data;
	t_lookup *lut = &x->lut;
	t_cloud *cloud = &x->cloud;
	float threshold = x->threshold;
	float *f, *f2, *frgb = x->rgb;
	float4 vec;
	
	const float xscale = 1.f/DEPTH_WIDTH;
	const float yscale = 1.f/DEPTH_HEIGHT;
	const float colscale = 1.f / 255.f;
	
	if(!source){
		return;	
	}
	
	if(!out_bp || !dest_info){
		error("Invalid pointer in copy_depth_data.");
		return;
	}
	
	in = source;
	in2 = source + DEPTH_WIDTH;
	
	cloud->count = 0;
	
	threshold *= threshold;
	
	c = rgb;
	f = frgb;
	
	
	for(i=0;i<DEPTH_HEIGHT;i++){
		for(j=0;j<(DEPTH_WIDTH*3);j+=4){
			vec[0] = (float)c[0];
			vec[1] = (float)c[1];
			vec[2] = (float)c[2];
			vec[3] = (float)c[3];
			mult_scalar_float4(vec, colscale, f);
			c+=4;
			f+=4;
		}
	}
	
	f = frgb;
	f2 = frgb + (3 * DEPTH_WIDTH);
		
	for(i=0,i2=1;i<(DEPTH_HEIGHT-1);i++,i2++){
		
		valid = 0;
		
		for(j=0;j<DEPTH_WIDTH;j++){
			
			if(*in != 0x7FF){
				pd = d;
				d = lut->f_ptr[*in];
				if(valid){
					dd = pd - d; dd*=dd;
					if(dd < threshold){
						push_cloud_point(cloud, xlut[j] * d, ylut[i] * d, d * -1.f, 
										 (float)j*xscale, (float)i*yscale,
										 0.f,0.f,-1.f,
										 f[0],f[1],f[2],1.f);
					}
					else{
						terminate_cloud_point(cloud);
						start_cloud_point(cloud, xlut[j] * d, ylut[i] * d, d * -1.f, 
										  (float)j*xscale, (float)i*yscale,
										  0.f,0.f,-1.f,
										  f[0],f[1],f[2],1.f);
					}
				}
				else{
					start_cloud_point(cloud, xlut[j] * d, ylut[i] * d, d * -1.f, 
									  (float)j*xscale, (float)i*yscale,
									  0.f,0.f,-1.f,
									  f[0],f[1],f[2],1.f);
					valid = 1;
				}
				if(*in2 != 0x7FF){
					d2 = lut->f_ptr[*in2];
					dd = d2 - d; dd*=dd;
					if(dd < threshold){
						push_cloud_point(cloud, xlut[j] * d2, ylut[i2] * d2, d2 * -1.f, 
										 (float)j*xscale, (float)i*yscale,
										 0.f,0.f,-1.f,
										 f2[0],f2[1],f2[2],1.f);
					}
					else{
						terminate_cloud_point(cloud);
					}
				}
				else{
					terminate_cloud_point(cloud);
				}
			}
			else if(valid){
				terminate_cloud_point(cloud);
				valid = 0;
			}
			in++;
			in2++;
			f+=3;
			f2+=3;
		}
		
		if(cloud->count && valid)terminate_cloud_point(cloud);
	}
	 	
	dest_info->type = _jit_sym_float32;
	dest_info->planecount = 12;
	dest_info->dimcount = 1;
	dest_info->dim[0] = cloud->count;
	dest_info->dimstride[0] = sizeof(t_point3D);
	dest_info->flags = JIT_MATRIX_DATA_REFERENCE | JIT_MATRIX_DATA_FLAGS_USE;
	jit_object_method(matrix,_jit_sym_setinfo_ex,dest_info);
	jit_object_method(matrix,_jit_sym_data,cloud->points);
	
}
*/

void copy_rgb_data(uint8_t *source, char *out_bp, t_jit_matrix_info *dest_info)
{
	int i,j;
	
	char *out;
	uint8_t *in;
	
	if(!source){
		return;
	}
	
	if(!out_bp || !dest_info){
		error("Invalid pointer in copy_depth_data.");
		return;
	}
	
	in = source;
	
	if(dest_info->planecount == 4){
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
	else if(dest_info->planecount == 1){
		for(i=0;i<RGB_HEIGHT;i++){
			out = out_bp + dest_info->dimstride[1] * i;
			for(j=0;j<RGB_WIDTH;j++){
				*out = *in;
				
				out ++;
				in ++;
			}
		}
	}
}

void rgb_callback(freenect_device *dev, void *pixels, uint32_t timestamp){
	t_jit_freenect_grab *x;
	
	x = freenect_get_user(dev);
	
	if(!x)return;
    
    pthread_mutex_lock(&x->cb_mutex);
	
	x->rgb_data = pixels;
	x->rgb_timestamp = timestamp;
	x->have_rgb_frames++;
    
    pthread_mutex_unlock(&x->cb_mutex);
}

void depth_callback(freenect_device *dev, void *pixels, uint32_t timestamp){
	t_jit_freenect_grab *x;
	
	x = freenect_get_user(dev);
	
	if(!x)return;
    
    pthread_mutex_lock(&x->cb_mutex);
	
	x->depth_data = pixels;
	x->depth_timestamp = timestamp;
	x->have_depth_frames++;
    
    pthread_mutex_unlock(&x->cb_mutex);
}
