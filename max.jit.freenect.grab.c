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


#include "jit.common.h"
#include "max.jit.mop.h"

#include <time.h>

typedef struct _max_jit_freenect_grab 
{
	t_object		ob;
	void			*obex;
	t_atom			*av;
} t_max_jit_freenect_grab;

t_jit_err jit_freenect_grab_init(void); 
void *max_jit_freenect_grab_new(t_symbol *s, long argc, t_atom *argv);
void max_jit_freenect_grab_free(t_max_jit_freenect_grab *x);
void max_jit_freenect_grab_outputmatrix(t_max_jit_freenect_grab *x);

void *max_jit_freenect_grab_class;

t_symbol *ps_gethas_frames, *ps_getunique;

int main(void)
{	
	void *p,*q;
	
	union { void **v_ptr; t_messlist **m_ptr; } alias_ptr; //this is to avoid warnings when compiling as C++
	alias_ptr.v_ptr = &max_jit_freenect_grab_class;
	
	jit_freenect_grab_init(); //Initialize the Jitter object
	
	setup(alias_ptr.m_ptr, (method)max_jit_freenect_grab_new, (method)max_jit_freenect_grab_free, (short)sizeof(t_max_jit_freenect_grab), 
		0L, A_GIMME, 0);

	p = max_jit_classex_setup(calcoffset(t_max_jit_freenect_grab,obex));
	q = jit_class_findbyname(gensym("jit_freenect_grab"));    
    max_jit_classex_mop_wrap(p,q,MAX_JIT_MOP_FLAGS_OWN_OUTPUTMATRIX|MAX_JIT_MOP_FLAGS_OWN_JIT_MATRIX);		
    max_jit_classex_standard_wrap(p,q,0); 	
	max_addmethod_usurp_low((method)max_jit_freenect_grab_outputmatrix, "outputmatrix");
    addmess((method)max_jit_mop_assist, "assist", A_CANT,0);
	
	ps_gethas_frames = gensym("gethas_frames");
	ps_getunique = gensym("getunique");
	
	return 0;
}

void max_jit_freenect_grab_outputmatrix(t_max_jit_freenect_grab *x)
{
	t_jit_err err;
	
	long ac;
	char output;
	
	long outputmode = max_jit_mop_getoutputmode(x);
	void *mop = max_jit_obex_adornment_get(x,_jit_sym_jit_mop);
	void *o;
	
	if (outputmode && mop){
		o = max_jit_obex_jitob_get(x);
		ac = 1;
		jit_object_method(o,ps_getunique,&ac,&(x->av));
		if(jit_atom_getlong(x->av)){
			ac = 1;
			jit_object_method(o,ps_gethas_frames,&ac,&(x->av));
			output = jit_atom_getlong(x->av);
		}else{
			output = 1;
		}
				
		jit_object_method(o,ps_gethas_frames,&ac,&(x->av));
		if(outputmode == 1){
			if(err = (t_jit_err)jit_object_method(
				max_jit_obex_jitob_get(x), 
				_jit_sym_matrix_calc,
				jit_object_method(mop,_jit_sym_getinputlist),
				jit_object_method(mop,_jit_sym_getoutputlist)))						
			{
				jit_error_code(x,err); 
			} else {
				if(output)
					max_jit_mop_outputmatrix(x);
			}
		} else {
			if(output)
				max_jit_mop_outputmatrix(x);
		}
	}	
}

void max_jit_freenect_grab_free(t_max_jit_freenect_grab *x)
{
	max_jit_mop_free(x);
	if(x->av){
		jit_freebytes(x->av, 1*sizeof(t_atom));	
	}
	jit_object_free(max_jit_obex_jitob_get(x));
	max_jit_obex_free(x);
}

void *max_jit_freenect_grab_new(t_symbol *s, long argc, t_atom *argv)
{
	t_max_jit_freenect_grab *x = NULL;
	void *o;

	if (x=(t_max_jit_freenect_grab *)max_jit_obex_new(max_jit_freenect_grab_class,gensym("jit_freenect_grab"))) {
		x->av = NULL;
		if (o=jit_object_new(gensym("jit_freenect_grab"))) {
			max_jit_mop_setup_simple(x,o,argc,argv);
			max_jit_attr_args(x,argc,argv);
			x->av = jit_getbytes(1*sizeof(t_atom));
			
			if(argc){
				if(argv[0].a_type == A_SYM){
					t_symbol *s = jit_atom_getsym(argv);
					if((s == _jit_sym_float32)||(s == _jit_sym_float64)||(s == _jit_sym_long)){
						//void *mop = max_jit_obex_adornment_get(x,_jit_sym_jit_mop);
						void *output = max_jit_mop_getoutput(x, 1);
						//jit_object_method(mop,_jit_sym_type, s);
						jit_attr_setsym(output, _jit_sym_type, s);
					}
					else{
						error("Invalid type argument: %s", argv[0].a_w.w_sym->s_name);
					}
				}
			}
		} else {
			error("jit.freenect.grab: could not allocate object");
			freeobject((t_object *)x);
		}
	}
	return (x);
}