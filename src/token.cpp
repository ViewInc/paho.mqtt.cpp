// token.cpp

/*******************************************************************************
 * Copyright (c) 2013-2019 Frank Pagliughi <fpagliughi@mindspring.com>
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution.
 *
 * The Eclipse Public License is available at
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Frank Pagliughi - initial implementation and documentation
 *    Frank Pagliughi - MQTT v5 support
 *******************************************************************************/

#include "mqtt/token.h"
#include "mqtt/async_client.h"
#include <cstring>

#include <iostream>

/////////////////////////////////////////////////////////////////////////////
// Paho C logger

enum LOG_LEVELS {
	INVALID_LEVEL = -1,
	TRACE_MAX = 1,
	TRACE_MED,
	TRACE_MIN,
	TRACE_PROTOCOL,
	LOG_PROTOCOL = TRACE_PROTOCOL,
	LOG_ERROR,
	LOG_SEVERE,
	LOG_FATAL,
};

extern "C" {
	void Log(enum LOG_LEVELS, int, const char *, ...);
}

/////////////////////////////////////////////////////////////////////////////

namespace mqtt {

// --------------------------------------------------------------------------
// Constructors

token::token(Type typ, iasync_client& cli, const_string_collection_ptr topics)
				: type_(typ), cli_(&cli), rc_(0), reasonCode_(ReasonCode::MQTT_SUCCESS),
						msgId_(MQTTAsync_token(0)), topics_(topics),
						userContext_(nullptr), listener_(nullptr), nExpected_(0),
						complete_(false)
{
}

token::token(Type typ, iasync_client& cli, const_string_collection_ptr topics,
			 void* userContext, iaction_listener& cb)
				: type_(typ), cli_(&cli), rc_(0), reasonCode_(ReasonCode::MQTT_SUCCESS),
						msgId_(MQTTAsync_token(0)), topics_(topics),
						userContext_(userContext), listener_(&cb), nExpected_(0),
						complete_(false)
{
}

token::token(Type typ, iasync_client& cli, MQTTAsync_token tok)
				: type_(typ), cli_(&cli), rc_(0), reasonCode_(ReasonCode::MQTT_SUCCESS),
					msgId_(tok), userContext_(nullptr),
					listener_(nullptr), nExpected_(0), complete_(false)
{
}

// --------------------------------------------------------------------------
// Class static callbacks.
// These are the callbacks directly from the C library.
// The 'context' is a raw pointer to the token object.

void token::on_success(void* context, MQTTAsync_successData* rsp)
{
	if (context)
		static_cast<token*>(context)->on_success(rsp);
}

void token::on_success5(void* context, MQTTAsync_successData5* rsp)
{
	if (context)
		static_cast<token*>(context)->on_success5(rsp);
}

void token::on_failure(void* context, MQTTAsync_failureData* rsp)
{
	if (context)
		static_cast<token*>(context)->on_failure(rsp);
}

void token::on_failure5(void* context, MQTTAsync_failureData5* rsp)
{
	if (context)
		static_cast<token*>(context)->on_failure5(rsp);
}

// --------------------------------------------------------------------------
// Object callbacks

//
// The success callback for MQTT v3 connections
//
void token::on_success(MQTTAsync_successData* rsp)
{
	::Log(TRACE_MIN, -1, "[cpp] on_success");

	unique_lock g(lock_);
	iaction_listener* listener = listener_;

	if (rsp) {
		msgId_ = rsp->token;

		switch (type_) {
			case Type::CONNECT:
				connRsp_.reset(new connect_response(rsp));
				break;

			case Type::SUBSCRIBE:
				subRsp_.reset(new subscribe_response(nExpected_, rsp));
				break;

			case Type::UNSUBSCRIBE:
				unsubRsp_.reset(new unsubscribe_response(rsp));
				break;

			default:
				// The others don't have responses
				break;
		}
	}

	rc_ = MQTTASYNC_SUCCESS;
	complete_ = true;
	g.unlock();

	// Note: callback always completes before the object is signaled.
	if (listener)
		listener->on_success(*this);
	cond_.notify_all();

	cli_->remove_token(this);
}

//
// The success callback for MQTT v5 connections
//
void token::on_success5(MQTTAsync_successData5* rsp)
{
	::Log(TRACE_MIN, -1, "[cpp] on_success5");

	unique_lock g(lock_);
	iaction_listener* listener = listener_;
	if (rsp) {
		msgId_ = rsp->token;
		reasonCode_ = ReasonCode(rsp->reasonCode);

		switch (type_) {
			case Type::CONNECT:
				connRsp_.reset(new connect_response(rsp));
				break;

			case Type::SUBSCRIBE:
				subRsp_.reset(new subscribe_response(rsp));
				break;

			case Type::UNSUBSCRIBE:
				unsubRsp_.reset(new unsubscribe_response(rsp));
				break;

			default:
				// The others don't have responses
				break;
		}
	}
	rc_ = MQTTASYNC_SUCCESS;
	complete_ = true;
	g.unlock();

	// Note: callback always completes before the object is signaled.
	if (listener)
		listener->on_success(*this);
	cond_.notify_all();

	cli_->remove_token(this);
}

//
// The failure callback for MQTT v3 connections
//
void token::on_failure(MQTTAsync_failureData* rsp)
{
	::Log(TRACE_MIN, -1, "[cpp] on_failure");

	unique_lock g(lock_);
	iaction_listener* listener = listener_;
	if (rsp) {
		msgId_ = rsp->token;
		rc_ = rsp->code;

		// HACK: For backward compatability with v3 connections
		reasonCode_ = ReasonCode(MQTTPP_V3_CODE);

		if (rsp->message)
			errMsg_ = string(rsp->message);
	}
	else {
		rc_ = -1;
	}
	complete_ = true;
	g.unlock();

	// Note: callback always completes before the obect is signaled.
	if (listener)
		listener->on_failure(*this);
	cond_.notify_all();

	cli_->remove_token(this);
}

//
// The failure callback for MQTT v5 connections
//
void token::on_failure5(MQTTAsync_failureData5* rsp)
{
	::Log(TRACE_MIN, -1, "[cpp] on_failure5");

	unique_lock g(lock_);
	iaction_listener* listener = listener_;
	if (rsp) {
		msgId_ = rsp->token;
		reasonCode_ = ReasonCode(rsp->reasonCode);
		//props_ = properties(rsp->properties);
		rc_ = rsp->code;
		if (rsp->message)
			errMsg_ = string(rsp->message);
	}
	else {
		rc_ = -1;
	}
	complete_ = true;
	g.unlock();

	// Note: callback always completes before the obect is signaled.
	if (listener)
		listener->on_failure(*this);
	cond_.notify_all();

	cli_->remove_token(this);
}

// --------------------------------------------------------------------------
// API

void token::reset()
{
	guard g(lock_);
	complete_ = false;
	rc_ = MQTTASYNC_SUCCESS;
	reasonCode_ = ReasonCode::MQTT_SUCCESS;
	errMsg_.clear();
}

void token::wait()
{
	unique_lock g(lock_);
	cond_.wait(g, [this]{return complete_;});
	check_ret();
}

connect_response token::get_connect_response() const
{
	if (type_ != Type::CONNECT)
		throw bad_cast();

	unique_lock g(lock_);
	cond_.wait(g, [this]{return complete_;});
	check_ret();

	if (!connRsp_)
		throw missing_response("connect");

	return *connRsp_;
}

subscribe_response token::get_subscribe_response() const
{
	if (type_ != Type::SUBSCRIBE)
		throw bad_cast();

	unique_lock g(lock_);
	cond_.wait(g, [this]{return complete_;});
	check_ret();

	if (!subRsp_)
		throw missing_response("subscribe");

	return *subRsp_;
}

unsubscribe_response token::get_unsubscribe_response() const
{
	if (type_ != Type::UNSUBSCRIBE)
		throw bad_cast();

	unique_lock g(lock_);
	cond_.wait(g, [this]{return complete_;});
	check_ret();

	if (!unsubRsp_)
		throw missing_response("unsubscribe");

	return *unsubRsp_;
}

/////////////////////////////////////////////////////////////////////////////
// end namespace mqtt
}

