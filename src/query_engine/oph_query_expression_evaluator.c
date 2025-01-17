/*
    Ophidia IO Server
    Copyright (C) 2014-2022 CMCC Foundation

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "oph_query_expression_evaluator.h"
#include "oph_query_expression_functions.h"
#include "oph_query_expression_parser.h"
#include "oph_query_expression_lexer.h"
#include "oph_query_engine_log_error_codes.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <debug.h>

//Global
extern int msglevel;
extern oph_query_expr_symtable *oph_function_table;

#define MIN_VAR_ARRAY_LENGTH 20

static oph_query_expr_node *allocate_node()
{
	oph_query_expr_node *b = (oph_query_expr_node *) malloc(sizeof(oph_query_expr_node));

	if (b == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		return NULL;
	}
	//set a temporary value and type
	b->type = eVALUE;
	b->left = NULL;
	b->right = NULL;

	return b;
}

oph_query_expr_node *oph_query_expr_create_double(double value)
{
	oph_query_expr_node *b = allocate_node();

	if (b == NULL)
		//no space for allocation
		return NULL;

	b->type = eVALUE;
	b->value.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
	b->value.free_flag = 0;
	b->value.jump_flag = 0;
	b->value.data.double_value = value;

	return b;
}

oph_query_expr_node *oph_query_expr_create_long(long long value)
{
	oph_query_expr_node *b = allocate_node();

	if (b == NULL)
		//no space for allocation
		return NULL;

	b->type = eVALUE;
	b->value.type = OPH_QUERY_EXPR_TYPE_LONG;
	b->value.free_flag = 0;
	b->value.jump_flag = 0;
	b->value.data.long_value = value;

	return b;
}

oph_query_expr_node *oph_query_expr_create_null()
{
	oph_query_expr_node *b = allocate_node();

	if (b == NULL)
		//no space for allocation
		return NULL;

	b->type = eVALUE;
	b->value.type = OPH_QUERY_EXPR_TYPE_NULL;
	b->value.jump_flag = 0;
	b->value.free_flag = 0;

	return b;
}

oph_query_expr_node *oph_query_expr_create_string(char *value)
{
	oph_query_expr_node *b = allocate_node();

	if (b == NULL)
		//no space for allocation
		return NULL;

	b->type = eVALUE;
	b->value.type = OPH_QUERY_EXPR_TYPE_STRING;
	b->value.free_flag = 0;
	b->value.jump_flag = 0;
	b->value.data.string_value = value;

	return b;
}

oph_query_expr_node *oph_query_expr_create_variable(char *name)
{

	oph_query_expr_node *b = allocate_node();

	if (b == NULL)
		//no space for allocation
		return NULL;

	b->type = eVAR;
	b->name = name;

	return b;
}

oph_query_expr_node *oph_query_expr_create_function(char *name, oph_query_expr_node * args)
{
	oph_query_expr_node *b = allocate_node();

	if (b == NULL)
		//no space for allocation
		return NULL;

	b->type = eFUN;
	b->name = name;
	b->descriptor.initialized = 0;
	b->descriptor.aggregate = 0;
	b->descriptor.clear = 0;
	b->descriptor.dlh = NULL;
	b->descriptor.initid = NULL;
	b->descriptor.internal_args = NULL;
	b->left = args;
	b->right = NULL;

	return b;
}

oph_query_expr_node *oph_query_expr_create_operation(oph_query_expr_node_type type, oph_query_expr_node * left, oph_query_expr_node * right)
{
	oph_query_expr_node *b = allocate_node();

	if (b == NULL)
		//no space for allocation
		return NULL;

	b->type = type;
	b->left = left;
	b->right = right;

	return b;
}

int oph_query_expr_delete_node(oph_query_expr_node * b, oph_query_expr_symtable * table)
{
	//base case for recursion and error case if null pointer is passed by user
	if (b == NULL)
		return OPH_QUERY_ENGINE_NULL_PARAM;

	if (b->type == eFUN) {
		oph_query_expr_record *r = oph_query_expr_lookup(b->name, oph_function_table);
		if (r == NULL)
			r = oph_query_expr_lookup(b->name, table);
		if (table != NULL && r != NULL && r->type == 2) {
			int er = 1;
			r->function(NULL, 0, b->name, &(b->descriptor), 1, &er);
		}
	}
	//free type-specific values
	if (b->type == eVAR || b->type == eFUN) {
		free(b->name);
	}

	if (b->type == eVALUE && b->value.type == OPH_QUERY_EXPR_TYPE_STRING) {
		free(b->value.data.string_value);
	}
	//call function recursevely nodes
	oph_query_expr_delete_node(b->left, table);
	oph_query_expr_delete_node(b->right, table);

	//free current node
	free(b);
	return OPH_QUERY_ENGINE_SUCCESS;
}

int oph_query_expr_create_function_symtable(int additional_size)
{
	if (additional_size < 0) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}
	int MIN_SIZE = 7;	///<< should equal be equal to number of built-in functions added to symtable
	oph_function_table = (oph_query_expr_symtable *) malloc(sizeof(oph_query_expr_symtable));

	if (oph_function_table == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}

    /**
     *the size of the array needs to be equal the number of built-in function/variables and
     *the number of function that the user plans to add to the symtable by end (specified in 
     *the paramenter additional_size)
     */
	oph_function_table->maxSize = MIN_SIZE + additional_size;

	oph_function_table->array = (oph_query_expr_record **) calloc(oph_function_table->maxSize, sizeof(oph_query_expr_record *));
	if ((oph_function_table->array) == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		free(oph_function_table);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}
	//add all the built-in variable and functions (if adding new built-in remember to change MIN_SIZE)
	//oph_query_expr_add_variable("b",-1,(*table));

	oph_query_expr_add_function("oph_id", 0, 2, oph_id, oph_function_table);
	oph_query_expr_add_function("oph_id2", 0, 3, oph_id2, oph_function_table);
	oph_query_expr_add_function("oph_id3", 0, 3, oph_id3, oph_function_table);
	oph_query_expr_add_function("oph_is_in_subset", 0, 4, oph_is_in_subset, oph_function_table);
	oph_query_expr_add_function("oph_id_to_index2", 0, 3, oph_id_to_index2, oph_function_table);
	oph_query_expr_add_function("oph_id_to_index", 1, 2, oph_id_to_index, oph_function_table);
	oph_query_expr_add_function("one", 0, 2, oph_query_generic_double, oph_function_table);
	return OPH_QUERY_ENGINE_SUCCESS;
}


int oph_query_expr_create_symtable(oph_query_expr_symtable ** table, int additional_size)
{
	if (table == NULL || additional_size < 0) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}
	int MIN_SIZE = 0;	///<< should equal be equal to number of built-in functions added to symtable
	(*table) = (oph_query_expr_symtable *) malloc(sizeof(oph_query_expr_symtable));

	if ((*table) == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}

    /**
     *the size of the array needs to be equal the number of built-in function/variables and
     *the number of function that the user plans to add to the symtable by end (specified in 
     *the paramenter additional_size)
     */
	(*table)->maxSize = MIN_SIZE + additional_size;

	(*table)->array = (oph_query_expr_record **) calloc((*table)->maxSize, sizeof(oph_query_expr_record *));
	if (((*table)->array) == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		free(*table);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}

	return OPH_QUERY_ENGINE_SUCCESS;
}

int oph_query_expr_destroy_symtable(oph_query_expr_symtable * table)
{
	if (table == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}

	int max = table->maxSize;
	int i = 0;
	for (; i < max; i = i + 1) {
		if (table->array[i] != NULL) {
			free(table->array[i]->name);
			if (table->array[i]->type == 1 && table->array[i]->value.type == OPH_QUERY_EXPR_TYPE_STRING) {
				free(table->array[i]->value.data.string_value);
			}
			free(table->array[i]);
		}
	}
	free(table->array);
	free(table);
	return OPH_QUERY_ENGINE_SUCCESS;
}

oph_query_expr_record *oph_query_expr_lookup(const char *s, oph_query_expr_symtable * table)
{
	//make sure pointer is not null
	if (table == NULL) {
		return NULL;
	}
	//go thought array and look for name    
	int i = 0;
	for (; i < table->maxSize; i = i + 1) {
		if (table->array[i] != NULL && strcmp(table->array[i]->name, s) == 0) {
			return table->array[i];
		}
	}
	return NULL;
}

int oph_query_expr_add_function(const char *name, int fun_type, int args_num, oph_query_expr_value(*value_fun) (oph_query_expr_value *, int, char *, oph_query_expr_udf_descriptor *, int, int *),
				oph_query_expr_symtable * table)
{
	if (table == NULL || args_num < 0 || value_fun == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}
	//create a record using the parameters infos
	oph_query_expr_record *sp;
	sp = (oph_query_expr_record *) malloc(sizeof(oph_query_expr_record));
	if (sp == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}
	sp->name = strdup(name);
	if (sp->name == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		free(sp);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}
	sp->type = 2;
	sp->fun_type = fun_type;
	sp->numArgs = args_num;
	sp->function = value_fun;

	//put a reference to the new record in the first empty spot in the array
	int max = table->maxSize;
	int i = 0;
	for (; i < max; i = i + 1) {
		if (table->array[i] == NULL) {
			table->array[i] = sp;
			return OPH_QUERY_ENGINE_SUCCESS;
		}
	}
    /**
	 *there is no more space available in the array. The user probably tried to add more fuction
	 *to the symtable than the number specified when he created it. Or adding a new built-in 
     *someone forgot to change MIN_SIZE.
     */
	pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_FULL_SYMTABLE);
	logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_FULL_SYMTABLE);
	return OPH_QUERY_ENGINE_MEMORY_ERROR;
}

int oph_query_expr_add_variable(const char *name, oph_query_expr_value_type var_type, double double_value, long long long_value,
				char *string_value, oph_query_arg * binary_value, oph_query_expr_symtable * table)
{

	if (table == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}
	//create a record
	oph_query_expr_record *sp;
	sp = (oph_query_expr_record *) malloc(sizeof(oph_query_expr_record));
	if (sp == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}
	sp->name = strdup(name);
	if (sp->name == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		free(sp);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}

	sp->type = 1;
	sp->value.free_flag = 0;
	sp->value.jump_flag = 0;

	switch (var_type) {
		case OPH_QUERY_EXPR_TYPE_DOUBLE:
			{
				sp->value.data.double_value = double_value;
				sp->value.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
				break;
			}
		case OPH_QUERY_EXPR_TYPE_LONG:
			{
				sp->value.data.long_value = long_value;
				sp->value.type = OPH_QUERY_EXPR_TYPE_LONG;
				break;

			}
		case OPH_QUERY_EXPR_TYPE_STRING:
			{
				sp->value.data.string_value = strdup(string_value);
				sp->value.type = OPH_QUERY_EXPR_TYPE_STRING;
				break;
			}
		case OPH_QUERY_EXPR_TYPE_BINARY:
			{
				sp->value.data.binary_value = binary_value;
				sp->value.type = OPH_QUERY_EXPR_TYPE_BINARY;
				break;
			}
		case OPH_QUERY_EXPR_TYPE_NULL:
			{
				sp->value.type = OPH_QUERY_EXPR_TYPE_NULL;
				break;
			}
		default:
			{
				pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_UNKNOWN_TYPE);
				logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_UNKNOWN_TYPE);
				return OPH_QUERY_ENGINE_MEMORY_ERROR;
			}
	}

	int max = table->maxSize;
	int i = 0;
	for (; i < max; i = i + 1) {
		if (table->array[i] == NULL) {
			table->array[i] = sp;
			return OPH_QUERY_ENGINE_SUCCESS;
		}

		if (table->array[i]->type == 1 && strcmp(table->array[i]->name, name) == 0) {
			table->array[i]->value = sp->value;
			free(sp->name);
			free(sp);
			return OPH_QUERY_ENGINE_SUCCESS;
		}
	}
	pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_FULL_SYMTABLE);
	logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_FULL_SYMTABLE);
	return OPH_QUERY_ENGINE_MEMORY_ERROR;
}

int oph_query_expr_add_double(const char *name, double value, oph_query_expr_symtable * table)
{
	return oph_query_expr_add_variable(name, OPH_QUERY_EXPR_TYPE_DOUBLE, value, 0, NULL, NULL, table);
}

int oph_query_expr_add_long(const char *name, long long value, oph_query_expr_symtable * table)
{
	return oph_query_expr_add_variable(name, OPH_QUERY_EXPR_TYPE_LONG, 0, value, NULL, NULL, table);
}

int oph_query_expr_add_string(const char *name, char *value, oph_query_expr_symtable * table)
{
	//redirect to more general function
	return oph_query_expr_add_variable(name, OPH_QUERY_EXPR_TYPE_STRING, 0, 0, value, NULL, table);
}

int oph_query_expr_add_binary(const char *name, oph_query_arg * value, oph_query_expr_symtable * table)
{
	return oph_query_expr_add_variable(name, OPH_QUERY_EXPR_TYPE_BINARY, 0, 0, NULL, value, table);
}

int eeparse(int mode, oph_query_expr_node ** expression, yyscan_t scanner);

oph_query_expr_value *get_array_args(char *name, oph_query_expr_node * e, int fun_type, int num_args_required, int *num_args_used, int *er, oph_query_expr_symtable * table, char *jump_flag);

int oph_query_expr_get_ast(const char *expr, oph_query_expr_node ** e)
{
	if (expr == NULL || e == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}
	oph_query_expr_node *expression;
	yyscan_t scanner;
	YY_BUFFER_STATE state;

	//initiate scanner
	if (eelex_init(&scanner)) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_LEXER_INIT_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_LEXER_INIT_ERROR);
		return OPH_QUERY_ENGINE_ERROR;
	}

	state = ee_scan_string(expr, scanner);

	if (eeparse(0, &expression, scanner)) {
		ee_delete_buffer(state, scanner);
		eelex_destroy(scanner);
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_QUERY_PARSING_ERROR, expr);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_QUERY_PARSING_ERROR, expr);
		return OPH_QUERY_ENGINE_PARSE_ERROR;
	}
	ee_delete_buffer(state, scanner);
	state = ee_scan_string(expr, scanner);
	eeparse(1, &expression, scanner);
	ee_delete_buffer(state, scanner);
	eelex_destroy(scanner);
	(*e) = expression;
	return OPH_QUERY_ENGINE_SUCCESS;
}

double get_double_value(oph_query_expr_value value, int *er, const char *fun_name)
{
	if (value.type == OPH_QUERY_EXPR_TYPE_DOUBLE) {
		return value.data.double_value;
	} else if (value.type == OPH_QUERY_EXPR_TYPE_LONG) {
		return (double) value.data.long_value;
	} else {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_TYPE_ERROR, fun_name, "double or long");
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_TYPE_ERROR, fun_name, "double or long");
		*er = -1;
		return 1;
	}
}

long long get_long_value(oph_query_expr_value value, int *er, const char *fun_name)
{
	if (value.type == OPH_QUERY_EXPR_TYPE_LONG) {
		return value.data.long_value;
	} else {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_TYPE_ERROR, fun_name, "long");
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_TYPE_ERROR, fun_name, "long");
		*er = -1;
		return 1;
	}
}


char *get_string_value(oph_query_expr_value value, int *er, const char *fun_name)
{
	if (value.type == OPH_QUERY_EXPR_TYPE_STRING) {
		return value.data.string_value;
	} else {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_TYPE_ERROR, fun_name, "string");
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_TYPE_ERROR, fun_name, "string");
		*er = -1;
		return "";
	}
}

oph_query_arg *get_binary_value(oph_query_expr_value value, int *er, const char *fun_name)
{
	if (value.type == OPH_QUERY_EXPR_TYPE_BINARY) {
		return value.data.binary_value;
	} else {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_TYPE_ERROR, fun_name, "binary");
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_TYPE_ERROR, fun_name, "binary");
		*er = -1;
		return NULL;
	}
}

oph_query_expr_value evaluate(oph_query_expr_node * e, int *er, oph_query_expr_symtable * table)
{
	switch (e->type) {
		case eVALUE:
			{
				return e->value;
			}
		case eMULTIPLY:
			{
				oph_query_expr_value left = evaluate(e->left, er, table);
				oph_query_expr_value right = evaluate(e->right, er, table);
				double l = get_double_value(left, er, "*");
				double r = get_double_value(right, er, "*");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
				res.data.double_value = l * r;
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case ePLUS:
			{
				oph_query_expr_value left = evaluate(e->left, er, table);
				oph_query_expr_value right = evaluate(e->right, er, table);
				double l = get_double_value(left, er, "+");
				double r = get_double_value(right, er, "+");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
				res.data.double_value = l + r;
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eMINUS:
			{
				oph_query_expr_value left = evaluate(e->left, er, table);
				oph_query_expr_value right = evaluate(e->right, er, table);
				double l = get_double_value(left, er, "-");
				double r = get_double_value(right, er, "-");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
				res.data.double_value = l - r;
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eDIVIDE:
			{
				oph_query_expr_value left = evaluate(e->left, er, table);
				oph_query_expr_value right = evaluate(e->right, er, table);
				double l = get_double_value(left, er, "/");
				double r = get_double_value(right, er, "/");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
				res.data.double_value = l * r;
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eEQUAL:
			{
				oph_query_expr_value left = evaluate(e->left, er, table);
				oph_query_expr_value right = evaluate(e->right, er, table);
				double l = get_double_value(left, er, "=");
				double r = get_double_value(right, er, "=");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_LONG;
				res.data.long_value = (long long) (l == r);
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eMOD:
			{
				oph_query_expr_value left = evaluate(e->left, er, table);
				oph_query_expr_value right = evaluate(e->right, er, table);
				double l = get_double_value(left, er, "MOD");
				double r = get_double_value(right, er, "MOD");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_LONG;
				res.data.long_value = ((int) l % (int) r);
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eAND:
			{
				oph_query_expr_value left = evaluate(e->left, er, table);
				oph_query_expr_value right = evaluate(e->right, er, table);
				double l = get_double_value(left, er, "AND");
				double r = get_double_value(right, er, "AND");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_LONG;
				res.data.long_value = (long long) l && r;
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eOR:
			{
				oph_query_expr_value left = evaluate(e->left, er, table);
				oph_query_expr_value right = evaluate(e->right, er, table);
				double l = get_double_value(left, er, "OR");
				double r = get_double_value(right, er, "OR");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_LONG;
				res.data.long_value = (long long) (l || r);
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eNOT:
			{
				oph_query_expr_value right = evaluate(e->right, er, table);
				double r = get_double_value(right, er, "NOT");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_LONG;
				res.data.long_value = (long long) !r;
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eNEG:
			{
				oph_query_expr_value right = evaluate(e->right, er, table);
				double r = get_double_value(right, er, "NEG");
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
				res.data.double_value = -r;
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
		case eVAR:
			{
				oph_query_expr_record *r = oph_query_expr_lookup(e->name, table);
				if (r != NULL && r->type == 1) {
					return r->value;
				} else {
					pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_UNKNOWN_SYMBOL, e->name);
					logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_UNKNOWN_SYMBOL, e->name);
					*er = -1;
					oph_query_expr_value res;
					res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
					res.data.double_value = 0;
					res.free_flag = 0;
					res.jump_flag = 0;
					return res;
				}
			}
		case eFUN:
			{
				oph_query_expr_record *r = oph_query_expr_lookup(e->name, oph_function_table);
				if (r == NULL)
					r = oph_query_expr_lookup(e->name, table);
				if (r != NULL && r->type == 2) {
					int used_arg_num = 0;
					char jump_flag = 0;
					oph_query_expr_value *args = get_array_args(e->name, e->left, r->fun_type, r->numArgs, &used_arg_num, er, table, &jump_flag);
					if (jump_flag) {
						if (args) {
							//Remove intermediate computed values
							int i;
							for (i = 0; i < used_arg_num; i++) {
								if (args[i].free_flag) {
									switch (args[i].type) {
										case OPH_QUERY_EXPR_TYPE_STRING:
#ifdef PLUGIN_RES_COPY
											free(args[i].data.string_value);
#endif
											break;
										case OPH_QUERY_EXPR_TYPE_BINARY:
#ifdef PLUGIN_RES_COPY
											free(args[i].data.binary_value->arg);
#endif
											free(args[i].data.binary_value);
											break;
										case OPH_QUERY_EXPR_TYPE_DOUBLE:
										case OPH_QUERY_EXPR_TYPE_LONG:
										case OPH_QUERY_EXPR_TYPE_NULL:
											break;
									}
								}
							}
							free(args);
						}
						oph_query_expr_value res;
						res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
						res.data.double_value = 0;
						res.free_flag = 0;
						res.jump_flag = 1;
						return res;
					}
					if (args != NULL) {
						oph_query_expr_value res = r->function(args, used_arg_num, e->name, &(e->descriptor), 0, er);
						//Remove intermediate computed values
						int i;
						for (i = 0; i < used_arg_num; i++) {
							if (args[i].free_flag) {
								switch (args[i].type) {
									case OPH_QUERY_EXPR_TYPE_STRING:
#ifdef PLUGIN_RES_COPY
										free(args[i].data.string_value);
#endif
										break;
									case OPH_QUERY_EXPR_TYPE_BINARY:
#ifdef PLUGIN_RES_COPY
										free(args[i].data.binary_value->arg);
#endif
										free(args[i].data.binary_value);
										break;
									case OPH_QUERY_EXPR_TYPE_DOUBLE:
									case OPH_QUERY_EXPR_TYPE_LONG:
									case OPH_QUERY_EXPR_TYPE_NULL:
										break;
								}
							}

						}
						free(args);
						return res;
					} else {
						pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
						logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
						*er = -1;
						free(args);
						oph_query_expr_value res;
						res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
						res.data.double_value = 0;
						res.free_flag = 0;
						res.jump_flag = 0;
						return res;
					}
				} else {
					pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_UNKNOWN_SYMBOL, e->name);
					logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_UNKNOWN_SYMBOL, e->name);
					*er = -1;
					oph_query_expr_value res;
					res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
					res.data.double_value = 0;
					res.free_flag = 0;
					res.jump_flag = 0;
					return res;
				}
			}
		default:
			{
				*er = -1;
				oph_query_expr_value res;
				res.type = OPH_QUERY_EXPR_TYPE_DOUBLE;
				res.data.double_value = 0;
				res.free_flag = 0;
				res.jump_flag = 0;
				return res;
			}
	}
}

//helper of evaluate
oph_query_expr_value *get_array_args(char *name, oph_query_expr_node * e, int fun_type, int num_args_required, int *num_args_used, int *er, oph_query_expr_symtable * table, char *jump_flag)
{
	int num_args_provided = 0;
	oph_query_expr_node *cur = e;
	*jump_flag = 0;

	while (cur != NULL) {
		num_args_provided++;
		cur = cur->right;
	}
	//if the function has fixed num of param (type 0), error if required is different then provided
	if (!fun_type && num_args_provided != (num_args_required)) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_NUM_ERROR, name, num_args_required);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_NUM_ERROR, name, num_args_required);
		return NULL;
		//if the function has variable num of param (type 1), error if required is less then provided
	} else if (fun_type && num_args_provided < num_args_required) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_NUM_ERROR, name, num_args_required);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_ARG_NUM_ERROR, name, num_args_required);
		return NULL;
	}


	oph_query_expr_value *arr = (oph_query_expr_value *) calloc(num_args_provided, sizeof(oph_query_expr_value));
	cur = e;
	//the loop is reversed so they are put in the array in the same order they appeared in the query
	int i = num_args_provided - 1;
	for (; i >= 0; i--) {
		*er = 0;
		arr[i] = evaluate(cur->left, er, table);
		if (*er) {
			//In this case terminate execution of whole expression    
			*jump_flag = 0;
			free(arr);
			return NULL;
		}
		cur = cur->right;
		if (arr[i].jump_flag) {
			*jump_flag = 1;
		}
	}
	*num_args_used = num_args_provided;
	return arr;
}


int oph_query_expr_eval_expression(oph_query_expr_node * e, oph_query_expr_value ** res, oph_query_expr_symtable * table)
{
	if (e == NULL || res == NULL || table == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}

	oph_query_expr_value *result = (oph_query_expr_value *) malloc(sizeof(oph_query_expr_value));
	int *er = (int *) malloc(sizeof(int));
	if (er == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	}
	*er = 0;

	if (e != NULL) {
		*result = evaluate(e, er, table);
		if (*er != -1) {
			(*res) = result;
			free(er);
			return OPH_QUERY_ENGINE_SUCCESS;
		} else {
			free(er);
			free(result);
			pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_EVAL_ERROR);
			logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_EVAL_ERROR);
			return OPH_QUERY_ENGINE_PARSE_ERROR;
		}

	} else {
		free(er);
		free(result);
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_EVAL_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_EVAL_ERROR);
		return OPH_QUERY_ENGINE_PARSE_ERROR;
	}
}

int oph_query_expr_change_group(oph_query_expr_node * b)
{
	//base case for recursion and error case if null pointer is passed by user
	if (b == NULL) {
		pmesg(LOG_WARNING, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_WARNING, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}

	if (b->type == eFUN) {
		b->descriptor.clear = 1;
	}
	//call function recursevely nodes
	oph_query_expr_change_group(b->left);
	oph_query_expr_change_group(b->right);

	return OPH_QUERY_ENGINE_SUCCESS;
}

int oph_query_expr_get_variables_help(oph_query_expr_node * e, int *max_size, int *current_size, char ***names)
{
	if (!e)
		return OPH_QUERY_ENGINE_SUCCESS;
	if (!names) {
		pmesg(LOG_WARNING, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_WARNING, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}

	if ((*current_size) >= (*max_size)) {
		int new_size = (*current_size) << 1;
		char **temp = (char **) realloc(*names, new_size * sizeof(char *));
		if (temp == NULL) {
			pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
			logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_MEMORY_ALLOC_ERROR);
			return OPH_QUERY_ENGINE_MEMORY_ERROR;
		}
		*names = temp;
		*max_size = new_size;
	}

	if (e->type == eVAR) {
		int i;
		for (i = 0; i < *current_size; i++)
			if (!strcmp((*names)[i], e->name))
				break;
		if (i >= *current_size)
			(*names)[(*current_size)++] = e->name;
	}

	if (oph_query_expr_get_variables_help(e->right, max_size, current_size, names))
		return OPH_QUERY_ENGINE_MEMORY_ERROR;
	if (oph_query_expr_get_variables_help(e->left, max_size, current_size, names))
		return OPH_QUERY_ENGINE_MEMORY_ERROR;

	return OPH_QUERY_ENGINE_SUCCESS;
}

int oph_query_expr_get_variables(oph_query_expr_node * e, char ***var_list, int *var_count)
{
	if (e == NULL || var_list == NULL || var_count == NULL) {
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_NULL_INPUT_PARAM);
		return OPH_QUERY_ENGINE_NULL_PARAM;
	}

	int max_size = MIN_VAR_ARRAY_LENGTH;
	int current_size = 0;
	char **names = calloc(sizeof(char *), max_size);
	if (oph_query_expr_get_variables_help(e, &max_size, &current_size, &names) != 0) {
		free(names);
		pmesg(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_VAR_EXTRACTION_ERROR);
		logging(LOG_ERROR, __FILE__, __LINE__, OPH_QUERY_ENGINE_LOG_VAR_EXTRACTION_ERROR);
		return OPH_QUERY_ENGINE_EXEC_ERROR;
	}
	*var_count = current_size;
	*var_list = names;
	return OPH_QUERY_ENGINE_SUCCESS;
}
