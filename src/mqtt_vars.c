/*==============================================================================
MIT License

Copyright (c) 2023 Trevor Monk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
==============================================================================*/
/*!
 * @defgroup mqttvars mqttvars
 * @brief Send VarServer variables to MQTT broker using paho_mqtt
 * @{
 */

/*============================================================================*/
/*!
@file mqttvars.c

    MQTT Variables

    The mqttvars Application maps variables to an MQTT broker
    using a JSON object definition to describe the pub/sub mapping

    Variables and their mappings are defined in
    a JSON object as follows:

    {
        "mqttvars" : [
            { "var" : "/sys/test/a",
              "qos" : 1,
              "mapping":"ps" },
            { "var" : "/sys/test/b",
              "qos" : 2,
              "mapping":"p" }
        ]
    }

    Variables with a mapping including 'p' (publish) will be written to the
    MQTT broker when the variable changes.

    Variables with a mapping including 's' (subscribe) will be subscribed
    to receive a notification from the MQTT broker when the value changes.

*/
/*============================================================================*/

/*==============================================================================
        Includes
==============================================================================*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <varserver/varserver.h>
#include <varserver/varobject.h>
#include <tjson/json.h>
#include <pthread.h>
#include <MQTTAsync.h>

/*==============================================================================
        Private definitions
==============================================================================*/

/*! default broker address */
#define ADDRESS     "tcp://test.mosquitto.org:1883"

/*! default MQTT client */
#define CLIENTID    "MQTTVarsClient"

/*! Publish flag */
#define PUBLISH  (1 << 0)

/*! subscribe flag */
#define SUBSCRIBE ( 1 << 1 )

/*! mqttVar component which maps a system variable to
 *  a command sequence */
typedef struct mqttVar
{
    /*! variable handle */
    VAR_HANDLE hVar;

    /*! MQTT topic */
    char *topic;

    /*! qos */
    int qos;

    /*! publish/subscribe */
    int pubsub;

    /*! pointer to a VarObject to cache the most recent data
     *  for this variable */
    VarObject obj;

    /*! pointer to the next variable */
    struct mqttVar *pNext;
} MqttVar;


/*! MqttVars state */
typedef struct mqttVarsState
{
    /*! variable server handle */
    VARSERVER_HANDLE hVarServer;

    /*! verbose flag */
    bool verbose;

    /*! name of the MqttVars definition file */
    char *pFileName;

    /*! pointer to the file vars list */
    MqttVar *pMqttVars;

    /*! Asynchronous MQTT client */
    MQTTAsync client;

    /*! connection options */
    MQTTAsync_connectOptions conn_opts;

    /*! flag to indicate process completed */
    int finished;

    /*! flag to indicate if the mqtt client is connected */
    bool connected;

    /*! array of subscription topic qos values */
    int *subscriptionQoS;

    /*! array of subscription topics */
    char **subscriptionTopics;

    /*! number of subscriptions */
    int numSubscriptions;

    /*! MQTT broker address */
    char *address;

    /*! MQTT username */
    char *username;

    /*! MQTT password */
    char *password;

    /*! MQTT client ID */
    char *clientID;

    /*! signal set to block on */
    sigset_t set;

} MqttVarsState;

/*==============================================================================
        Private file scoped variables
==============================================================================*/

/*! MqttVars State object */
MqttVarsState state;

volatile MQTTAsync_token deliveredtoken;

/*==============================================================================
        Private function declarations
==============================================================================*/

void main(int argc, char **argv);
static int ProcessOptions( int argC, char *argV[], MqttVarsState *pState );
static void usage( char *cmdname );
static int SetupMqttVar( JNode *pNode, void *arg );
static void SetupTerminationHandler( void );
static int OptionVariable( MqttVarsState *pState, char **option );
static int SetSigMask( sigset_t *set );
static int WaitSignal( int *sigval );
static void *sig_thread(void *arg);
static void TerminationHandler( int signum, siginfo_t *info, void *ptr );

static int MQTTInit( MqttVarsState *pState );
static int MQTTSend( MqttVarsState *pState, VAR_HANDLE hVar );
static int MQTTSubscribe( MqttVarsState *pState );
static MqttVar *MQTTFindVar( MqttVarsState *pState, VAR_HANDLE hVar );
static MqttVar *MQTTFindTopic( MqttVarsState *pState,
                               char *topicName,
                               int topicLen );
static int MQTTBuildSubscriptionList( MqttVarsState *pState );
static int MQTTPayload( char *src, int srclen, char *dst, int dstlen );

static int MQTTRead( MqttVarsState *pState,
                     MqttVar *pVar,
                     char *payload,
                     int payloadlen );

void onConnect(void* context, MQTTAsync_successData* response);
void onConnectFailure(void* context, MQTTAsync_failureData* response);
void onSend(void* context, MQTTAsync_successData* response);
void onSubscribe(void* context, MQTTAsync_successData* response);
void onSubscribeFailure(void* context, MQTTAsync_failureData* response);
void onDisconnect(void* context, MQTTAsync_successData* response);
void connlost(void *context, char *cause);
void onDeliveryComplete( void *context, MQTTAsync_token token );
int onMessageArrived( void *context,
                      char *topicName,
                      int topicLen,
                      MQTTAsync_message *message );

/*==============================================================================
        Private function definitions
==============================================================================*/

/*============================================================================*/
/*  main                                                                      */
/*!
    Main entry point for the mqttvars application

    The main function starts the mqttvars application

    @param[in]
        argc
            number of arguments on the command line
            (including the command itself)

    @param[in]
        argv
            array of pointers to the command line arguments

    @return none

==============================================================================*/
void main(int argc, char **argv)
{
    VARSERVER_HANDLE hVarServer = NULL;
    VAR_HANDLE hVar;
    int result;
    JNode *config;
    JArray *vars;
    int sigval;
    int fd;
    int sig;
    int rc;
    char buf[BUFSIZ];
    pthread_t thread;

    MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;

    /* clear the mqttvars state object */
    memset( &state, 0, sizeof( state ) );

    SetSigMask( &state.set );
    state.address = ADDRESS;
    state.clientID = CLIENTID;

    if( argc < 2 )
    {
        usage( argv[0] );
        exit( 1 );
    }

    /* set up an abnormal termination handler */
    SetupTerminationHandler();

    /* process the command line options */
    ProcessOptions( argc, argv, &state );

    /* process the input file */
    config = JSON_Process( state.pFileName );

    /* get the configuration array */
    vars = (JArray *)JSON_Find( config, "mqttvars" );

    /* set up MQTT connection options */
    state.conn_opts = conn_opts;


    /* get a handle to the VAR server */
    state.hVarServer = VARSERVER_Open();
    if( state.hVarServer != NULL )
    {
        MQTTInit( &state );

        /* set up the file vars by iterating through the configuration array */
        JSON_Iterate( vars, SetupMqttVar, (void *)&state );

        /* build subscription list */
        MQTTBuildSubscriptionList( &state );

        rc = pthread_create( &thread, NULL, &sig_thread, &state );

        while( !state.finished )
        {
            sleep(1);
        }

        printf("MQTTVars: Shutting down!\n");

        /* shut down the MQTT client */
        MQTTAsync_destroy(&state.client);

        /* close the variable server */
        VARSERVER_Close( state.hVarServer );
    }
}

/*============================================================================*/
/*  SetupMqttVar                                                              */
/*!
    Set up an MqttVar object

    The SetupMqttVar function is a callback function for the JSON_Iterate
    function which sets up an mqtt variable from the JSON configuration.
    The file variable definition object is expected to look as follows:

    { "var" : "/sys/test/a", "qos" : 1, "mapping":"ps" }

    @param[in]
       pNode
            pointer to the MqttVar node

    @param[in]
        arg
            opaque pointer argument used for the mqttvar state object

    @retval EOK - the mqtt variable was set up successfully
    @retval EINVAL - the mqtt variable could not be set up

==============================================================================*/
static int SetupMqttVar( JNode *pNode, void *arg )
{
    MqttVarsState *pState = (MqttVarsState *)arg;
    char *varname = NULL;
    char *mapping;
    int qos = 0;
    char *topic = NULL;
    VARSERVER_HANDLE hVarServer;
    VAR_HANDLE hVar;
    MqttVar *pMqttvar;
    int result = EINVAL;

    if( pState != NULL )
    {
        /* get a handle to the VarServer */
        hVarServer = pState->hVarServer;
        varname = JSON_GetStr( pNode, "var" );
        mapping = JSON_GetStr( pNode, "mapping" );
        JSON_GetNum( pNode, "qos", &qos );

        topic = strdup(varname);

        hVar = VAR_FindByName( hVarServer, varname );

        if( ( hVar != VAR_INVALID ) &&
            ( topic != NULL ) &&
            ( mapping != NULL ) )
        {
            /* allocate memory for the file variable */
            pMqttvar = calloc( 1, sizeof( MqttVar ) );
            if( pMqttvar != NULL )
            {
                pMqttvar->hVar = hVar;
                pMqttvar->topic = topic;
                pMqttvar->qos = qos;

                /* get the variable */
                VAR_Get( pState->hVarServer,
                         hVar,
                         &pMqttvar->obj );

                /* check if we are publishing this variable */
                if ( strchr(mapping, 'p') != NULL )
                {
                    pMqttvar->pubsub |= PUBLISH;
                }

                /* check if we are subscribing to this variable */
                if ( strchr(mapping, 's') != NULL )
                {
                    pMqttvar->pubsub |= SUBSCRIBE;
                    pState->numSubscriptions++;
                }

                /* set up modified notification for published variables */
                if ( pMqttvar->pubsub & PUBLISH )
                {
                    result = VAR_Notify( hVarServer,
                                         pMqttvar->hVar,
                                         NOTIFY_MODIFIED );
                    if ( result != EOK )
                    {
                        fprintf( stderr,
                                 "Failed to set up notification for %s\n",
                                 varname );
                    }
                    else if ( pState->verbose )
                    {
                        printf( "Registered for notifications on %s\n",
                                varname );
                    }
                }

                pMqttvar->pNext = pState->pMqttVars;
                pState->pMqttVars = pMqttvar;
            }
        }
        else
        {
            fprintf(stderr, "Failed to setup %s\n", varname );
        }
    }

    return result;
}

/*============================================================================*/
/*  usage                                                                     */
/*!
    Display the application usage

    The usage function dumps the application usage message
    to stderr.

    @param[in]
       cmdname
            pointer to the invoked command name

    @return none

==============================================================================*/
static void usage( char *cmdname )
{
    if( cmdname != NULL )
    {
        fprintf(stderr,
                "usage: %s [-v] [-h]"
                "[-a address] [-i id] [-u user] [-p passwd]\n"
                " [-h] : display this help\n"
                " [-v] : verbose output\n"
                " -f <filename> : configuration file\n"
                " -a <address> : broker address\n"
                " -i <clientId> : clientId\n"
                " -u <userName> : client user name\n"
                " -p <password> : client password\n",
                cmdname );
    }
}

/*============================================================================*/
/*  ProcessOptions                                                            */
/*!
    Process the command line options

    The ProcessOptions function processes the command line options and
    populates the MqttVarState object

    @param[in]
        argC
            number of arguments
            (including the command itself)

    @param[in]
        argv
            array of pointers to the command line arguments

    @param[in]
        pState
            pointer to the MqttVars state object

    @return none

==============================================================================*/
static int ProcessOptions( int argC, char *argV[], MqttVarsState *pState )
{
    int c;
    int result = EINVAL;
    const char *options = "hvf:a:i:u:p:";

    if( ( pState != NULL ) &&
        ( argV != NULL ) )
    {
        while( ( c = getopt( argC, argV, options ) ) != -1 )
        {
            switch( c )
            {
                case 'v':
                    pState->verbose = true;
                    break;

                case 'h':
                    usage( argV[0] );
                    break;

                case 'f':
                    pState->pFileName = strdup(optarg);
                    break;

                case 'a':
                    pState->address = strdup(optarg);
                    break;

                case 'i':
                    pState->clientID = strdup(optarg);
                    break;

                case 'u':
                    pState->username = strdup(optarg);
                    break;

                case 'p':
                    pState->password = strdup(optarg);
                    break;

                default:
                    break;

            }
        }
    }

    return 0;
}

/*============================================================================*/
/*  SetSigMask                                                                */
/*!
    Set thread signal mask for VarServer signals

    The SetSigMask sets up the thread signal mask to receive signals from
    the variable server.  Allowed signals are: SIG_VAR_MODIFIED, SIG_VAR_CALC,
    SIG_VAR_PRINT, and SIG_VAR_VALIDATE.

    @param[in]
        set
            pointer to the sigset_t to add the signals to

    @retval EINVAL invalid arguments
    @retval other result of pthread_sigmask call

==============================================================================*/
static int SetSigMask( sigset_t *set )
{
    int result = EINVAL;

    if ( set != NULL )
    {
        sigemptyset( set );

        /* modified notification */
        sigaddset( set, SIG_VAR_MODIFIED );
        /* calc notification */
        sigaddset( set, SIG_VAR_CALC );
        /* validate notification */
        sigaddset( set, SIG_VAR_PRINT );
        /* print notification */
        sigaddset( set, SIG_VAR_VALIDATE );

        /* block on these signals */
        result = pthread_sigmask( SIG_BLOCK, set, NULL );
    }

    return result;
}

/*============================================================================*/
/*  WaitSignal                                                                */
/*!
    Wait for a signal from the Variable Server

    The WaitSignal function waits for one of the following signals from
    the VarServer:  SIG_VAR_MODIFIED, SIG_VAR_CALC, SIG_VAR_PRINT, or
    SIG_VAR_VALIDATE.

    The function blocks until one of the expected signals is received,
    and returns the signal value in the specified sigval storage location

    @param[in]
        sigval
            pointer to a location to store the received signal value

    @retval EINVAL invalid arguments
    @retval other result of pthread_sigmask call

==============================================================================*/
static int WaitSignal( int *sigval )
{
    sigset_t mask;
    siginfo_t info;
    int sig;

    /* initialize empty signal set */
    sigemptyset( &mask );

    /* modified notification */
    sigaddset( &mask, SIG_VAR_MODIFIED );
    /* calc notification */
    sigaddset( &mask, SIG_VAR_CALC );
    /* validate notification */
    sigaddset( &mask, SIG_VAR_PRINT );
    /* print notification */
    sigaddset( &mask, SIG_VAR_VALIDATE );

    /* block on these signals */
    pthread_sigmask( SIG_BLOCK, &mask, NULL );

    /* wait for a signal */
    sig = sigwaitinfo( &mask, &info );

    if( sigval != NULL )
    {
        *sigval = info.si_value.sival_int;
    }

    return sig;
}

/*============================================================================*/
/*  sig_thread                                                                */
/*!
    The sig_thread task waits for a received signal.
    It assumes the received signal is a SIG_VAR_MODIFIED signal from
    the variable server.  It gets the variable handle from the sigval,
    and calls MQTTSend to send the variable value to the MQTT broker.

    This thread runs while the finished flag in the MqttVarsState object
    is false.

    @param[in]
        arg
            void pointer to the MqttVarsState object

    @return none

==============================================================================*/
static void *sig_thread(void *arg)
{
    MqttVarsState *pState = (MqttVarsState *)arg;
    siginfo_t info;
    int sig;
    int sigval;
    VAR_HANDLE hVar;

    while (!pState->finished )
    {
        sig = sigwaitinfo( &pState->set, &info );
        sigval = info.si_value.sival_int;
        hVar = (VAR_HANDLE)sigval;
        MQTTSend( &state, hVar );
    }
}

/*============================================================================*/
/*  OptionVariable                                                            */
/*!
    The OptionVariable function checks if a command line option contains
    a VarServer variable name.  If an option starts with a $ it is assumed
    to be a variable, and the variable value is looked up and substituted
    in place of the variable name.

    @param[in]
        pState
            pointer to the MqttVarsState object

    @param[in]
        option
            pointer to an option to be evaluated

    @retval EINVAL invalid arguments
    @retval ENOENT variable was not found
    @retval EOK option was substituted ok or no substitution was needed

==============================================================================*/
static int OptionVariable( MqttVarsState *pState, char **option )
{
    int result = EINVAL;
    char *pOption;
    VarObject obj;
    VAR_HANDLE hVar;
    int rc;

    if ( ( pState != NULL ) && ( option != NULL ) )
    {
        result = EOK;
        pOption = *option;

        memset( &obj, 0, sizeof(VarObject) );

        if ( pOption != NULL )
        {
            if ( pOption[0] == '$' )
            {
                /* get a handle to the variable */
                hVar = VAR_FindByName( pState->hVarServer, &pOption[1] );
                if ( hVar != VAR_INVALID )
                {
                    /* get the variable value and type */
                    rc = VAR_Get( pState->hVarServer, hVar, &obj );
                    if ( ( rc == EOK ) && ( obj.type == VARTYPE_STR ) )
                    {
                        /* free the memory used by the variable name */
                        free( pOption );

                        /* update the option value */
                        *option = obj.val.str;
                        result = EOK;
                    }
                }
                else
                {
                    result = ENOENT;
                }
            }
            else
            {
                /* nothing to do */
                result = EOK;
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  SetupTerminationHandler                                                   */
/*!
    Set up an abnormal termination handler

    The SetupTerminationHandler function registers a termination handler
    function with the kernel in case of an abnormal termination of this
    process.

==============================================================================*/
static void SetupTerminationHandler( void )
{
    static struct sigaction sigact;

    memset( &sigact, 0, sizeof(sigact) );

    sigact.sa_sigaction = TerminationHandler;
    sigact.sa_flags = SA_SIGINFO;

    sigaction( SIGTERM, &sigact, NULL );
    sigaction( SIGINT, &sigact, NULL );

}

/*============================================================================*/
/*  TerminationHandler                                                        */
/*!
    Abnormal termination handler

    The TerminationHandler function will be invoked in case of an abnormal
    termination of this process.  The termination handler closes
    the connection with the variable server and cleans up its VARFP shared
    memory.

@param[in]
    signum
        The signal which caused the abnormal termination (unused)

@param[in]
    info
        pointer to a siginfo_t object (unused)

@param[in]
    ptr
        signal context information (ucontext_t) (unused)

==============================================================================*/
static void TerminationHandler( int signum, siginfo_t *info, void *ptr )
{
    syslog( LOG_ERR, "Abnormal termination of mqttvars\n" );
    state.finished = true;
}

/*============================================================================*/
/*  MQTTInit                                                                  */
/*!
    Initialize internal state and connect to the MQTT broker

    The MQTTInit function processes the command line options,
    initializes the MqttVarsState object, creates an MQTT client,
    and connects to the MQTT broker.  It uses the MQTTAsync mechanisms
    supplied by paho_mqtt

    @param[in]
        pState
            pointer to the MqttVarsState object

    @return 0

==============================================================================*/
static int MQTTInit( MqttVarsState *pState )
{
    int rc;

    if ( pState != NULL )
    {
        /* get MQTT command line options */
        OptionVariable( pState, &pState->address );
        OptionVariable( pState, &pState->username );
        OptionVariable( pState, &pState->password );
        OptionVariable( pState, &pState->clientID );

        if ( pState->verbose )
        {
            printf("creating client\n");
            printf("connecting to: %s as %s\n",
                    pState->address,
                    pState->username );
        }

        MQTTAsync_create( &(pState->client),
                          pState->address,
                          pState->clientID,
                          MQTTCLIENT_PERSISTENCE_NONE,
                          NULL );

        MQTTAsync_setCallbacks( pState->client,
                                pState,
                                connlost,
                                onMessageArrived, /* message arrived */
                                onDeliveryComplete /* delivery complete */ );

        if ( pState->username != NULL )
        {
            pState->conn_opts.username = pState->username;
        }

        if ( pState->password != NULL )
        {
            pState->conn_opts.password = pState->password;
        }

        pState->conn_opts.keepAliveInterval = 20;
        pState->conn_opts.cleansession = 1;
        pState->conn_opts.onSuccess = onConnect;
        pState->conn_opts.onFailure = onConnectFailure;
        pState->conn_opts.context = pState;

        rc = MQTTAsync_connect( pState->client, &pState->conn_opts );
        if (rc != MQTTASYNC_SUCCESS)
        {
            printf("Failed to start connect, return code %d\n", rc);
            exit(EXIT_FAILURE);
        }
    }

    return 0;
}

/*============================================================================*/
/*  MQTTBuildSubscriptionList                                                 */
/*!
    Build a subscription list for all of the variables which
    we are subscribing to.  For each variable, a quality of service
    and topic entry are created.

    @param[in]
        pState
            pointer to the MqttVarsState object

    @return number of variables we are subscribing to

==============================================================================*/
static int MQTTBuildSubscriptionList( MqttVarsState *pState )
{
    MqttVar *pMqttVar = NULL;
    int n;
    int i=0;

    if ( pState != NULL )
    {
        n = pState->numSubscriptions;
        if ( n > 0 )
        {
            /* allocate memory for the QoS array */
            pState->subscriptionQoS = calloc( n, sizeof( int ) );

            /* allocate memeory for the subscription topics array */
            pState->subscriptionTopics = calloc( n, sizeof( char *) );

            if ( ( pState->subscriptionQoS != NULL ) &&
                 ( pState->subscriptionTopics != NULL ) )
            {
                /* iterate through the variables and populate
                 * the subscription QoS and topics */
                pMqttVar = pState->pMqttVars;
                while( pMqttVar != NULL )
                {
                    if ( pMqttVar->pubsub & SUBSCRIBE )
                    {
                        pState->subscriptionQoS[i] = pMqttVar->qos;
                        pState->subscriptionTopics[i] = pMqttVar->topic;
                        i++;
                    }

                    pMqttVar = pMqttVar->pNext;
                }
            }
        }
    }

    return i;
}

/*============================================================================*/
/*  MQTTSubscribe                                                             */
/*!
    Subscribe to all of the variables in the MQTT subscription list

    The MQTTSubscribe function uses the paho_mqtt MQTTAsync_subscribeMany
    function to subscribe to all of the variables in the MQTT subscription
    list.

    @param[in]
        pState
            pointer to the MqttVarsState object containing the MQTT subscription
            list

    @retval EINVAL invalid arguments
    @retval EOK subscription set up ok
    @retval other failure from MQTTASync_subscribeMany

==============================================================================*/
static int MQTTSubscribe( MqttVarsState *pState )
{
    MqttVar *pMqttVar = NULL;
    int result = EINVAL;
    int rc;
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    int i;

    if ( pState != NULL )
    {
        result = EOK;

        opts.onSuccess = onSubscribe;
        opts.onFailure = onSubscribeFailure;
        opts.context = pState;

        result = MQTTAsync_subscribeMany(
                    pState->client,
                    pState->numSubscriptions,
                    pState->subscriptionTopics,
                    pState->subscriptionQoS,
                    &opts );

        if ( pState->verbose )
        {
            for( i=0; i< pState->numSubscriptions; i++ )
            {
                printf( "%s:%d\n",
                        pState->subscriptionTopics[i],
                        pState->subscriptionQoS[i] );
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  MQTTFindVar                                                               */
/*!
    Find a variable in the MQTT variable list

    The MQTTFindVar function iterates through the MQTT variable list
    looking for a variable with the specified handle.

    @param[in]
        pState
            pointer to the MqttVarsState object containing the MQTT variables

    @param[in]
        hVar
            handle to the variable to search for

    @retval pointer to the MqttVar which was found
    @retval NULL if no MqttVar was found

==============================================================================*/
static MqttVar *MQTTFindVar( MqttVarsState *pState, VAR_HANDLE hVar )
{
    MqttVar *pMqttVar = NULL;

    if ( pState != NULL )
    {
        pMqttVar = pState->pMqttVars;
        while ( pMqttVar != NULL )
        {
            if ( pMqttVar->hVar == hVar )
            {
                break;
            }

            pMqttVar = pMqttVar->pNext;
        }
    }

    return pMqttVar;
}

/*============================================================================*/
/*  MQTTFindTopic                                                             */
/*!
    Find a topic in the MQTT variable list

    The MQTTFindTopic function iterates through the MQTT variable list
    looking for the specified topic.

    @param[in]
        pState
            pointer to the MqttVarsState object containing the MQTT variables

    @param[in]
        topicName
            pointer to the topic name to search for

    @param[in]
        topicLen
            length of the topic name to search for

    @retval pointer to the MqttVar which was found
    @retval NULL if no MqttVar was found

==============================================================================*/
static MqttVar *MQTTFindTopic( MqttVarsState *pState,
                               char *topicName,
                               int topicLen )
{
    MqttVar *pMqttVar = NULL;
    int len;

    if ( pState != NULL )
    {
        if ( len == 0 )
        {
            len = strlen( topicName );
        }

        pMqttVar = pState->pMqttVars;
        while ( pMqttVar != NULL )
        {
            if ( strncmp( pMqttVar->topic, topicName, len ) == 0 )
            {
                break;
            }

            pMqttVar = pMqttVar->pNext;
        }
    }

    return pMqttVar;
}

/*============================================================================*/
/*  MQTTRead                                                                  */
/*!
    Read a VarServer variable from a received MQTT payload

    The MQTTRead function converts a received MQTT payload into a
    VarServer variable value using VAROBJECT_ValueFromString for
    numeric variable types, and string copy for string types.

    @param[in]
        pState
            pointer to the MqttVarsState object containing the MQTT variables

    @param[in]
        pVar
            pointer to the MqttVar to populate from the payload

    @param[in]
        payload
            pointer to the received MQTT payload

    @param[in]
        payloadlen
            length of the received MQTT payload

    @retval EINVAL invalid arguments
    @retval EOK payload was extracted successfully
    @retval ENOTSUP payload type not supported

==============================================================================*/
static int MQTTRead( MqttVarsState *pState,
                     MqttVar *pVar,
                     char *payload,
                     int payloadlen )
{
    int result = EINVAL;
    VarType type;
    char buf[BUFSIZ];
    int rc;
    bool update = false;
    VarObject *pObj;

    if ( ( pState != NULL ) &&
         ( pVar != NULL ) &&
         ( payload != NULL ) &&
         ( payloadlen > 0 ) )
    {
        result = EOK;

        pObj = &pVar->obj;

        if ( pObj != NULL )
        {
            type = pObj->type;
            if ( type != VARTYPE_STR )
            {
                /* get the MQTT payload and NUL terminate */
                rc = MQTTPayload( payload, payloadlen, buf, sizeof(buf) );
                if ( rc == EOK )
                {
                    rc = VAROBJECT_ValueFromString( buf, pObj, 0 );
                    if ( rc == EOK )
                    {
                        update = true;
                    }
                }
            }
            else
            {
                if ( pObj->val.str != NULL )
                {
                    if( payloadlen < pObj->len )
                    {
                        rc = strncmp( payload, pObj->val.str, payloadlen );
                        if( rc != 0 )
                        {
                            rc = MQTTPayload( payload,
                                              payloadlen,
                                              pObj->val.str,
                                              pObj->len );
                            if ( rc == EOK )
                            {
                                update = true;
                            }
                        }
                    }
                }
            }

            result = update ? EOK : ENOTSUP;
        }
    }

    return result;

}

/*============================================================================*/
/*  MQTTPayload                                                               */
/*!
    Copy an MQTT Payload

    The MQTTPayload function copies the specified MQTT Payload into the
    specified buffer.

    @param[in]
        src
            pointer to the source data

    @param[in]
        srclen
            length of the source data

    @param[in]
        dst
            pointer to the destination buffer

    @param[in]
        dstlen
            length of the destination buffer

    @retval EINVAL invalid arguments
    @retval EOK payload was copied successfully

==============================================================================*/
static int MQTTPayload( char *src, int srclen, char *dst, int dstlen )
{
    int result = EINVAL;

    if ( ( src != NULL ) && ( dst != NULL ) && ( dstlen > srclen ) )
    {
        /* copy data */
        memcpy(dst, src, srclen );

        /* NUL terminate */
        dst[srclen] = 0;

        result = EOK;
    }

    return result;
}

/*============================================================================*/
/*  MQTTSend                                                                  */
/*!
    Send an MQTT payload to the broker

    The MQTTSend function builds an MQTT payload from the specified MQTT
    Variable and sends it to the MQTT broker.

    @param[in]
        pState
            pointer to the MqttVarsState object

    @param[in]
        hVar
            handle to the variable to send to the MQTT broker

    @retval EINVAL failed to send payload
    @retval EOK payload send successfully

==============================================================================*/
static int MQTTSend( MqttVarsState *pState,
                     VAR_HANDLE hVar)
{
    int result = EINVAL;
    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    MQTTAsync_message pubmsg = MQTTAsync_message_initializer;
    int rc;
    char name[256];
    char buf[256];
    char *p;
    MqttVar *pMqttVar;

    if ( pState != NULL )
    {
        if ( pState->connected == true )
        {
            opts.onSuccess = onSend;
            opts.context = pState;

            pMqttVar = MQTTFindVar( pState, hVar );
            if ( pMqttVar != NULL )
            {
                /* get the variable object */
                VAR_Get( pState->hVarServer,
                         hVar,
                         &(pMqttVar->obj) );

                p = buf;

                /* convert the variable object to a string payload */
                VAROBJECT_ToString( &(pMqttVar->obj), buf, sizeof(buf) );

                pubmsg.payload = p;
                pubmsg.payloadlen = strlen(p);
                pubmsg.qos = pMqttVar->qos;
                pubmsg.retained = 0;
                deliveredtoken = 0;

                if ( pState->verbose )
                {
                    printf("Send message to %s\n", pMqttVar->topic );
                }

                /* send the payload to the MQTT broker */
                rc = MQTTAsync_sendMessage(pState->client,
                                           pMqttVar->topic,
                                           &pubmsg,
                                           &opts);
                if (rc != MQTTASYNC_SUCCESS )
                {
                    printf("Failed to start sendMessage, return code %d\n", rc);
                    result = EINVAL;
                }
                else
                {
                    result = EOK;
                }
            }
            else
            {
                printf("Cannot send message when disconnected\n");
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  onDeliveryComplete                                                        */
/*!
    MQTT Delivery Complete callback

    The onDeliveryComplete callback function is called by the paho_mqtt
    library when a payload has been successfully delivered to the MQTT broker

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        token
            token associated with the successful delivery

==============================================================================*/
void onDeliveryComplete( void *context, MQTTAsync_token token )
{
    MqttVarsState *pState = (MqttVarsState *)context;

    if ( pState != NULL )
    {
        if ( pState->verbose )
        {
            printf("Delivery complete: token=%d\n", token );
        }
    }
}

/*============================================================================*/
/*  onMessageArrived                                                          */
/*!
    MQTT Message Arrived callback

    The onMessageArrived callback function is called by the paho_mqtt
    library when an MQTT payload has been received.  This function
    finds the VarServer variable associated with the received MQTT
    payload and updates the variable value.

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        topicName
            name of the topic that has been received

    @param[in]
        topicLen
            length of the topic name

    @param[in]
        message
            pointer to the received MQTT message

    @return 1

==============================================================================*/
int onMessageArrived( void *context,
                      char *topicName,
                      int topicLen,
                      MQTTAsync_message *message )
{

    MqttVarsState *pState = (MqttVarsState *)context;
    MqttVar *pVar;
    int len;
    int i;
    char* payloadptr;
    int oldlen;
    int rc;

    len = topicLen;

    if ( len == 0 )
    {
        len = strlen(topicName);
    }

    if ( pState->verbose )
    {
        payloadptr = message->payload;

        printf("Message arrived\n");
        printf("    topic: %s\n", topicName);
        printf("    Received message: %*s\n", len, topicName);
        printf("    value: %*s\n", message->payloadlen, payloadptr );

        for(i=0; i<message->payloadlen; i++)
        {
            putchar(*payloadptr++);
        }
        putchar('\n');
    }

    /* find the MQTT topic */
    pVar = MQTTFindTopic( pState, topicName, topicLen );
    if ( pVar != NULL )
    {
        /* read the payload */
        rc = MQTTRead( pState, pVar, message->payload, message->payloadlen );
        if ( rc == EOK )
        {
            if ( pVar->obj.type == VARTYPE_STR )
            {
                /* save the maximum string length */
                oldlen = pVar->obj.len;
                pVar->obj.len = message->payloadlen;
            }

            /* update the VarServer variable */
            VAR_Set( pState->hVarServer, pVar->hVar, &pVar->obj );

            if ( pVar->obj.type == VARTYPE_STR )
            {
                /* restore the old length */
                pVar->obj.len = oldlen;
            }
        }
    }

    /* free the received message and topic name */
    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topicName);

    return 1;
}

/*============================================================================*/
/*  connlist                                                                  */
/*!
    MQTT Connection List Callback

    The connlost callback function is called by the paho_mqtt
    library when an MQTT connection to the broker has been lost.

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        cause
            pointer to a string describing the cause of the lost connection

==============================================================================*/
void connlost(void *context, char *cause)
{
    MqttVarsState *pState = (MqttVarsState *)context;
    int rc;

    if ( pState->verbose )
    {
        printf("\nConnection lost\n");
        printf("     cause: %s\n", cause);
        printf("Reconnecting\n");
    }

    pState->conn_opts.keepAliveInterval = 20;
    pState->conn_opts.cleansession = 1;
    pState->connected = false;

    rc = MQTTAsync_connect( pState->client, &(pState->conn_opts) );
    if ( rc != MQTTASYNC_SUCCESS )
    {
        printf("Failed to start connect, return code %d\n", rc);
        pState->finished = 1;
    }
}

/*============================================================================*/
/*  onDisconnect                                                              */
/*!
    MQTT Disconnection Callback

    The onDisconnect callback function is called by the paho_mqtt
    library when an MQTT connection to the broker has disconnected

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        response
            pointer to a MQTTAsync_successData object

==============================================================================*/
void onDisconnect(void* context, MQTTAsync_successData* response)
{
    MqttVarsState *pState = (MqttVarsState *)context;

    if ( pState->verbose )
    {
        printf("Successful disconnection\n");
    }

    pState->connected = false;
    pState->finished = 1;
}

/*============================================================================*/
/*  onSubscribe                                                               */
/*!
    MQTT Subscription Success Callback

    The onSubscribe callback function is called by the paho_mqtt
    library when an MQTT subscription has completed successfully

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        response
            pointer to a MQTTAsync_successData object

==============================================================================*/
void onSubscribe(void* context, MQTTAsync_successData* response)
{
    MqttVarsState *pState = (MqttVarsState *)context;

    if ( pState->verbose )
    {
        printf("Subscription successful\n");
    }
}

/*============================================================================*/
/*  onSubscribeFailure                                                        */
/*!
    MQTT Subscription Failure Callback

    The onSubscribeFailure callback function is called by the paho_mqtt
    library when an MQTT subscription has completed unsuccessfully

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        response
            pointer to a MQTTAsync_failureData object

==============================================================================*/
void onSubscribeFailure(void* context, MQTTAsync_failureData* response)
{
    MqttVarsState *pState = (MqttVarsState *)context;
    printf("Subscription failed\n");
    pState->finished = 1;
}

/*============================================================================*/
/*  onSend                                                                    */
/*!
    MQTT Send Callback

    The onSend callback function is called by the paho_mqtt
    library when an MQTT topic has been successfully written to

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        response
            pointer to a MQTTAsync_successData object

==============================================================================*/
void onSend(void* context, MQTTAsync_successData* response)
{
    MqttVarsState *pState = (MqttVarsState *)context;

    if ( pState != NULL )
    {
        if ( pState->verbose )
        {
            printf("Message with token value %d delivery confirmed\n",
                    response->token);
        }
    }
}

/*============================================================================*/
/*  onConnectFailure                                                          */
/*!
    MQTT Connection Failure Callback

    The onConnectFailure callback function is called by the paho_mqtt
    library when an MQTT connection has failed to be established with the
    MQTT broker.

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        response
            pointer to a MQTTAsync_failureData object

==============================================================================*/
void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
    MqttVarsState *pState = (MqttVarsState *)context;
    printf("Connect failed, rc %d\n", response ? response->code : 0);
    pState->finished = 1;
    pState->connected = false;
}

/*============================================================================*/
/*  onConnect                                                                 */
/*!
    MQTT Connection Callback

    The onConnect callback function is called by the paho_mqtt
    library when an MQTT connection has been established with the
    MQTT broker.  This function sets up the MQTT topic subscriptions.

    @param[in]
        context
            void * pointer to the MqttVarsState object

    @param[in]
        response
            pointer to a MQTTAsync_successData object

==============================================================================*/
void onConnect(void* context, MQTTAsync_successData* response)
{
    MqttVarsState *pState = (MqttVarsState *)context;

    if ( pState->verbose )
    {
        printf("Successful connection\n");
    }

    MQTTSubscribe( pState );

    pState->connected = true;
}

/*! @}
 * end of mqttvars group */
